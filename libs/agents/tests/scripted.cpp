#include "scripted.hpp"

#include <fmt/format.h>

namespace kk::test {

namespace {

struct Provider final : llm::IProvider {
  ScriptedModel& m;
  std::string tier;
  Provider(ScriptedModel& model, std::string t) : m(model), tier(std::move(t)) {}
  kk::Result<llm::Response> send(const llm::Request& req) override {
    std::lock_guard lk(m.mu);
    m.calls.push_back({tier, req});
    if (m.script.empty()) return kk::fail("script", "script exhausted");
    auto f = std::move(m.script.front());
    m.script.pop_front();
    return f(req);
  }
  kk::Result<std::int64_t> count_tokens(const llm::Request&) override { return 0; }
  [[nodiscard]] std::string name() const override { return "anthropic"; }
};

int g_id = 0;

}  // namespace

llm::Response tool_response(const std::string& name, const json& input, llm::Usage u) {
  llm::Response r;
  r.stop_reason = "tool_use";
  llm::ContentBlock b;
  b.type = llm::ContentBlock::Type::ToolUse;
  b.id = fmt::format("toolu_{}", ++g_id);
  b.name = name;
  b.input_json = input.dump();
  r.content.push_back(b);
  r.usage = u;
  return r;
}

void ScriptedModel::then(std::function<llm::Response(const llm::Request&)> f) {
  std::lock_guard lk(mu);
  script.push_back(std::move(f));
}

void ScriptedModel::tool(const std::string& name, const json& input, llm::Usage u) {
  then([=](const llm::Request&) { return tool_response(name, input, u); });
}

void ScriptedModel::text(const std::string& t) {
  then([=](const llm::Request&) {
    llm::Response r;
    r.stop_reason = "end_turn";
    r.content.push_back(llm::ContentBlock::make_text(t));
    r.usage = {10, 5, 0, 0, 0};
    return r;
  });
}

agents::ProviderFactory ScriptedModel::factory() {
  return [this](const std::string& tier) -> kk::Result<std::unique_ptr<llm::IProvider>> {
    return std::unique_ptr<llm::IProvider>(std::make_unique<Provider>(*this, tier));
  };
}

std::string ScriptedModel::request_text(const llm::Request& r) {
  std::string out;
  for (const auto& b : r.system) out += b.text + "\n";
  for (const auto& m : r.messages) {
    for (const auto& b : m.content) out += b.text + b.input_json + "\n";
  }
  return out;
}

kk::Result<agents::TestRun> FakeTests::compile_check(const std::vector<std::string>& files) {
  std::lock_guard lk(mu);
  compile_calls.push_back(files);
  return agents::TestRun{true, true, "", {}, "compile check ok\n"};
}

kk::Result<agents::TestRun> FakeTests::build_and_test(const std::vector<std::string>& blocks, bool build_only) {
  std::lock_guard lk(mu);
  calls.push_back(blocks);
  if (build_only) return agents::TestRun{true, true, "", {}, "build ok\n"};
  if (fn) return fn(blocks);
  return agents::TestRun{true, true, "", {}, "tests ok\n"};
}

core::Config test_config() {
  core::Config c;
  for (const auto& [tier, model] : std::vector<std::pair<std::string, std::string>>{
           {"strong", "m-strong"}, {"medium", "m-medium"}, {"cheap", "m-cheap"}}) {
    core::ModelTier t;
    t.provider = "anthropic";
    t.model = model;
    t.fallbacks = "off";
    c.models[tier] = t;
  }
  c.prices["m-strong"] = {4.0, 20.0, 0.2, -1, -1};
  c.prices["m-medium"] = {2.0, 10.0, -1, -1, -1};
  c.prices["m-cheap"] = {0.1, 0.5, -1, -1, -1};
  c.max_steps_worker = 8;
  c.max_steps_tester = 8;
  // Scripted workers report done without writing files; the check has its own tests (ADR 0031).
  c.done_needs_change = false;
  c.max_steps_navigator = 4;
  c.max_steps_orchestrator = 4;
  c.max_steps_architect = 12;
  c.self_tune = false;
  return c;
}

}  // namespace kk::test
