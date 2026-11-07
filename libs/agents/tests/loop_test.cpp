#include <doctest/doctest.h>

#include "kk/agents/loop.hpp"
#include "kk/agents/tools.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;
using json = nlohmann::json;

namespace {
struct Host final : agents::IToolHost {
  int calls = 0;
  [[nodiscard]] std::vector<agents::Tool> tools() const override { return {agents::tool_def("read_file"), agents::tool_def("report_leaf")}; }
  agents::ToolOutcome call(const agents::ToolCall& c) override {
    ++calls;
    if (c.name == "report") return {"reported", false, true};
    return {"file text", false, false};
  }
};

agents::AgentSpec spec() {
  agents::AgentSpec s;
  s.role = agents::Role::Worker;
  s.block = "stream.ring";
  s.tier = "cheap";
  s.system_text = "SYSTEM";
  s.context_text = "STABLE CONTEXT";
  s.task_text = "TASK";
  s.max_steps = 5;
  return s;
}
}  // namespace

TEST_CASE("loop: tool use until a terminal tool; prefix order and moving cache marker (9.4, 9.6)") {
  test::TempDir d{"loop"};
  auto cfg = test::test_config();
  test::ScriptedModel m;
  m.tool("read_file", {{"path", "impl/ring.cpp"}}, {100, 10, 1000, 0, 0});
  m.tool("read_file", {{"path", "api/ring.hpp"}}, {20, 10, 0, 0, 1000});
  m.tool("report", {{"status", "fixed"}, {"cause", "off by one"}}, {20, 30, 0, 0, 1100});
  agents::AgentRunner runner(d.path(), cfg, m.factory(), nullptr);
  Host host;
  auto r = runner.run(spec(), host);
  CHECK(r.outcome == "terminal");
  CHECK(r.terminal_tool == "report");
  CHECK(json::parse(r.terminal_input)["cause"] == "off by one");
  CHECK(r.steps == 3);
  CHECK(r.tokens.input == 140);
  CHECK(r.tokens.output == 50);
  CHECK(r.tokens.cache_write_5m == 1000);
  CHECK(r.tokens.cache_read == 2100);
  CHECK(r.cost > 0.0);
  REQUIRE(m.calls.size() == 3);
  const auto& first = m.calls[0].req;
  CHECK(first.tools.size() == 2);
  CHECK(first.system[0].cache);
  REQUIRE(first.messages[0].content.size() == 2);
  CHECK(first.messages[0].content[0].text == "STABLE CONTEXT");
  CHECK(first.messages[0].content[0].cache);
  CHECK(first.messages[0].content[1].text == "TASK");
  CHECK_FALSE(first.messages[0].content[1].cache);
  const auto& third = m.calls[2].req;
  REQUIRE(third.messages.size() == 5);
  CHECK(third.messages[1].role == "assistant");
  CHECK(third.messages[4].content.back().type == llm::ContentBlock::Type::ToolResult);
  CHECK(third.messages[4].content.back().cache);
  // Journal: start, steps, end.
  auto j = test::read(d / ".karkas/runs" / r.journal);
  CHECK(core::contains(j, "\"type\":\"run_start\""));
  CHECK(core::contains(j, "\"kind\":\"tool\""));
  CHECK(core::contains(j, "\"type\":\"run_end\""));
  CHECK(core::contains(j, "\"outcome\":\"fixed\""));
}

TEST_CASE("loop: step limit, token budget, nudge and refusal") {
  test::TempDir d{"loop"};
  auto cfg = test::test_config();
  {
    test::ScriptedModel m;
    for (int i = 0; i < 10; ++i) m.tool("read_file", {{"path", "x"}});
    agents::AgentRunner runner(d.path(), cfg, m.factory(), nullptr);
    Host host;
    auto s = spec();
    s.max_steps = 3;
    auto r = runner.run(s, host);
    CHECK(r.outcome == "max_steps");
    CHECK(r.steps == 3);
  }
  {
    test::ScriptedModel m;
    for (int i = 0; i < 10; ++i) m.tool("read_file", {{"path", "x"}}, {60000, 1000, 0, 0, 0});
    agents::AgentRunner runner(d.path(), cfg, m.factory(), nullptr);
    Host host;
    auto s = spec();
    s.max_tokens = 100000;
    auto r = runner.run(s, host);
    CHECK(r.outcome == "budget");
    CHECK(r.steps == 2);
  }
  {
    test::ScriptedModel m;
    m.text("I think it is fixed.");
    m.tool("report", {{"status", "fixed"}});
    agents::AgentRunner runner(d.path(), cfg, m.factory(), nullptr);
    Host host;
    auto r = runner.run(spec(), host);
    CHECK(r.outcome == "terminal");
    CHECK(core::contains(test::ScriptedModel::request_text(m.calls[1].req), "Finish by calling one of your tools"));
  }
  {
    test::ScriptedModel m;
    m.then([](const llm::Request&) {
      llm::Response r;
      r.stop_reason = "refusal";
      return r;
    });
    agents::AgentRunner runner(d.path(), cfg, m.factory(), nullptr);
    Host host;
    CHECK(runner.run(spec(), host).outcome == "refusal");
  }
}

TEST_CASE("loop: a cloud model without a price never runs") {
  test::TempDir d{"loop"};
  auto cfg = test::test_config();
  cfg.prices.erase("m-cheap");
  test::ScriptedModel m;
  m.tool("report", {{"status", "fixed"}});
  agents::AgentRunner runner(d.path(), cfg, m.factory(), nullptr);
  Host host;
  auto r = runner.run(spec(), host);
  CHECK(r.outcome == "error");
  CHECK(core::contains(r.error, "no price"));
  CHECK(m.calls.empty());
}
