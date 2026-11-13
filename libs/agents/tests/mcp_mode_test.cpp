// Agents in mode "mcp" (docs/adr/0024): the same engine and protocol as with an API; an MCP
// client — here a thread playing Claude Code — is the model and answers every turn through the
// queue (kk_agent_next / kk_agent_act).
#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "kk/agents/engine.hpp"
#include "kk/agents/mcp_turns.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;
using json = nlohmann::json;

namespace {

std::string turn_id(const std::string& text) {
  // "TURN <id> · ..."
  if (!text.starts_with("TURN ")) return {};
  auto end = text.find(' ', 5);
  return text.substr(5, end == std::string::npos ? std::string::npos : end - 5);
}

}  // namespace

TEST_CASE("mcp mode: a bug ticket runs the same descent with an MCP client as the model") {
  test::FixtureCopy p{"wavkit"};
  test::FakeTests tests;
  auto cfg = test::test_config();
  cfg.agents_mode = "mcp";
  cfg.mcp_wait_s = 30;
  agents::EngineOptions o;  // no providers: the engine builds its own — the MCP bridge in this mode
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), cfg, o);
  REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));

  std::atomic<bool> done{false};
  std::vector<std::string> seen;  // who was served, in order
  std::string worker_after_read;  // what the worker saw after its read_file
  std::string worker_delta;       // its next turn through kk_agent_next(run, after)
  std::thread client([&] {
    agents::McpTurns turns(p.path());
    while (!done) {
      auto text = turns.next("", 1, false);
      auto id = turn_id(text);
      if (id.empty()) continue;
      auto call = [&](const json& calls) {
        auto r = turns.act(id, "", calls.dump(), 20);
        return r ? *r : r.error().message;
      };
      if (core::contains(text, "orchestrator at the project")) {
        seen.emplace_back("orchestrator");
        CHECK(core::contains(text, "=== SYSTEM ==="));
        CHECK(core::contains(text, "dispatch("));
        CHECK(core::contains(text, "Claude Code subagent karkas-strong"));
        auto r = call(json::array({{{"name", "dispatch"},
                                    {"input", {{"block", "stream"}, {"suspect", {"stream", "dsp"}}, {"difficulty", "M"},
                                               {"why", "frames dropped before dsp; ring runs empty"}}}}}));
        CHECK(core::contains(r, "ended"));
      } else if (core::contains(text, "navigator at stream")) {
        seen.emplace_back("navigator");
        call(json::array({{{"name", "dispatch"},
                           {"input", {{"block", "stream.ring"}, {"suspect", {"stream.ring", "stream.pump"}},
                                      {"ruled_out", {"stream.pump"}}, {"difficulty", "M"}, {"why", "drops at wraparound"}}}}}));
      } else if (core::contains(text, "worker at stream.ring")) {
        seen.emplace_back("worker");
        // An unknown tool is refused at once, before the engine sees it.
        auto bad = turns.act(id, "", json::array({{{"name", "rm_rf"}, {"input", json::object()}}}).dump(), 5);
        CHECK_FALSE(bad);
        // The worker reads its own file through the agent's tool, then reports.
        worker_after_read = call(json::array({{{"name", "read_file"}, {"input", {{"path", "impl/ring.cpp"}}}}}));
        auto next = turn_id(worker_after_read);
        REQUIRE_FALSE(next.empty());
        id = next;
        // An answer whose results are not ready within wait_s: the client comes back with
        // kk_agent_next(run, after=<its last turn>) and gets only what is new, not the whole turn.
        auto later = turns.act(id, "", json::array({{{"name", "list_dir"}, {"input", {{"path", "impl"}}}}}).dump(), 0);
        REQUIRE(later);
        CHECK(core::contains(*later, "after=\"" + id + "\""));
        auto run = id.substr(0, id.rfind('-')) + ".jsonl";
        auto fresh = turns.next(run, 20, false);  // a client new to the run (a subagent) gets all of it
        CHECK(core::contains(fresh, "=== SYSTEM ==="));
        worker_delta = turns.next(run, 20, false, id);
        id = turn_id(worker_delta);
        REQUIRE_FALSE(id.empty());
        call(json::array({{{"name", "report"},
                           {"input", {{"status", "fixed"}, {"cause", "off-by-one at wraparound"}, {"fix", "mask index with cap-1"},
                                      {"files", {"impl/ring.cpp"}}, {"api", "unchanged"}}}}}));
      }
    }
  });
  auto r = (*e)->debug("crackle in output after ~10 s of playback", {"contract stream.ring/no_drop FAIL"});
  done = true;
  client.join();
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));

  // The same protocol and trail as with an API (engine test "a bug descends the schema ...").
  auto t = test::read(p / ".karkas/tickets/T-0001.ksl");
  CHECK(core::contains(t, "status  closed"));
  CHECK(core::contains(t, "L0 orchestrator at=root suspect=stream,dsp pick=stream difficulty=M"));
  CHECK(core::contains(t, "L1 navigator    at=stream suspect=stream.ring,stream.pump ruled_out=stream.pump pick=stream.ring"));
  CHECK(core::contains(t, "outcome=fixed"));
  CHECK(core::contains(test::read(p / "blocks/stream/ring/block.ksl"), "hist    T-0001 off-by-one at wraparound -> mask index"));
  REQUIRE(seen.size() == 3);
  CHECK(seen[0] == "orchestrator");
  CHECK(seen[1] == "navigator");
  CHECK(seen[2] == "worker");
  // The read ran in the worker's sandbox and only its result came back (only what is new).
  CHECK(core::contains(worker_after_read, "=== NEW SINCE YOUR LAST ANSWER ==="));
  CHECK(core::contains(worker_after_read, "--- result of read_file ---"));
  CHECK(core::contains(worker_after_read, "std::atomic"));
  CHECK_FALSE(core::contains(worker_after_read, "=== SYSTEM ==="));
  CHECK(core::contains(worker_delta, "=== NEW SINCE YOUR LAST ANSWER ==="));
  CHECK(core::contains(worker_delta, "--- result of list_dir ---"));
  CHECK(core::contains(worker_delta, "ring.cpp"));
  CHECK_FALSE(core::contains(worker_delta, "=== SYSTEM ==="));
  // The journals say who answered: provider mcp, with an estimate of the client's tokens and cost
  // from the turns (ADR 0026), not a bill.
  auto runs = (*e)->index().runs();
  REQUIRE(runs.size() == 3);
  for (const auto& run : runs) {
    CHECK(run.provider == "mcp");
    CHECK(run.input_tokens > 0);
    CHECK(run.cost_usd > 0.0);
  }
  // Nothing is left in the queue.
  CHECK(llm::McpQueue(p / ".karkas" / "mcp").waiting().empty());
}

TEST_CASE("mcp mode: a turn nobody answers ends the run with a clear message") {
  test::FixtureCopy p{"wavkit"};
  test::FakeTests tests;
  auto cfg = test::test_config();
  cfg.agents_mode = "mcp";
  cfg.mcp_wait_s = 1;
  agents::EngineOptions o;
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), cfg, o);
  REQUIRE(e);
  auto r = (*e)->debug("crackle", {});
  // The orchestrator got no answer: the ticket is escalated with the reason, the queue is clean.
  auto t = test::read(p / ".karkas/tickets/T-0001.ksl");
  CHECK(core::contains(t, "no MCP client answered"));
  CHECK(llm::McpQueue(p / ".karkas" / "mcp").waiting().empty());
}

TEST_CASE("mcp mode: a run that asks another agent gets that nested run served by the same client") {
  test::FixtureCopy p{"wavkit"};
  test::FakeTests tests;
  auto cfg = test::test_config();
  cfg.agents_mode = "mcp";
  cfg.mcp_wait_s = 30;
  agents::EngineOptions o;
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), cfg, o);
  REQUIRE(e);
  std::atomic<bool> done{false};
  std::string nested_text;
  std::string orchestrator_again;
  std::thread client([&] {
    agents::McpTurns turns(p.path());
    std::string text;
    while (!done && turn_id(text).empty()) text = turns.next("", 1, false);
    if (done) return;
    auto orch = turn_id(text);
    // The orchestrator asks the agent of a child: the engine runs that agent inside the call.
    auto r = turns.act(orch, "", json::array({{{"name", "ask"}, {"input", {{"block", "stream"}, {"question", "Does the ring drop frames?"}}}}}).dump(), 20);
    REQUIRE(r);
    nested_text = *r;
    auto ask_turn = turn_id(nested_text.substr(nested_text.find("TURN ")));
    REQUIRE_FALSE(ask_turn.empty());
    auto a = turns.act(ask_turn, "", json::array({{{"name", "answer"}, {"input", {{"text", "yes, at the wraparound of the index"}}}}}).dump(), 20);
    REQUIRE(a);
    CHECK(core::contains(*a, "ended"));
    // Back to the orchestrator's run: its next turn carries the answer.
    std::string orch_run = orch.substr(0, orch.rfind('-')) + ".jsonl";
    orchestrator_again = turns.next(orch_run, 20, false);
    auto next = turn_id(orchestrator_again);
    REQUIRE_FALSE(next.empty());
    (void)turns.act(next, "", json::array({{{"name", "escalate"}, {"input", {{"note", "asked; a human decides"}}}}}).dump(), 20);
  });
  auto r = (*e)->debug("crackle", {});
  done = true;
  client.join();
  CHECK(core::contains(nested_text, "waits for this one"));
  CHECK(core::contains(nested_text, "ask at stream"));
  CHECK(core::contains(orchestrator_again, "yes, at the wraparound of the index"));
  auto t = test::read(p / ".karkas/tickets/T-0001.ksl");
  CHECK(core::contains(t, "asked; a human decides"));
  CHECK(llm::McpQueue(p / ".karkas" / "mcp").waiting().empty());
}

TEST_CASE("mcp mode: a new run of a role the client has served comes without the parts it has (seen)") {
  test::FixtureCopy p{"wavkit"};
  test::FakeTests tests;
  auto cfg = test::test_config();
  cfg.agents_mode = "mcp";
  cfg.mcp_wait_s = 30;
  agents::EngineOptions o;
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), cfg, o);
  REQUIRE(e);
  std::atomic<bool> done{false};
  std::string second_orchestrator;
  std::thread client([&] {
    agents::McpTurns turns(p.path());
    std::vector<std::string> served;
    int orchestrators = 0;
    while (!done) {
      auto text = turns.next("", 1, false, "", served);
      auto id = turn_id(text);
      if (id.empty()) continue;
      served.push_back(id.substr(0, id.rfind('-')) + ".jsonl");
      auto act = [&](const json& calls) { (void)turns.act(id, "", calls.dump(), 20); };
      if (core::contains(text, "orchestrator at the project")) {
        if (++orchestrators == 1) {
          act(json::array({{{"name", "dispatch"}, {"input", {{"block", "stream.ring"}, {"difficulty", "S"}, {"why", "ring part"}}}}}));
        } else {
          second_orchestrator = text;
          act(json::array({{{"name", "close"}, {"input", {{"verdict", "done"}, {"note", "in place"}}}}}));
        }
      } else if (core::contains(text, "worker at stream.ring")) {
        act(json::array({{{"name", "report"}, {"input", {{"status", "done"}, {"fix", "ring part done"}}}}}));
      }
    }
  });
  auto r = (*e)->task("tidy the ring");
  done = true;
  client.join();
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  // The second orchestrator run: no system prompt or tools again, the unchanged L0 view collapsed,
  // the trail (which changed) in full.
  REQUIRE_FALSE(second_orchestrator.empty());
  CHECK_FALSE(core::contains(second_orchestrator, "=== SYSTEM ==="));
  CHECK_FALSE(core::contains(second_orchestrator, "=== TOOLS"));
  CHECK(core::contains(second_orchestrator, "the same system prompt and tools (not repeated)"));
  CHECK(core::contains(second_orchestrator, "(unchanged since run "));
  CHECK(core::contains(second_orchestrator, "pick=stream.ring"));
  // The journals hold an estimate of the client's tokens from what went to it and back.
  auto runs = (*e)->index().runs();
  REQUIRE_FALSE(runs.empty());
  for (const auto& run : runs) {
    CHECK(run.provider == "mcp");
    CHECK(run.input_tokens > 0);
    CHECK(run.output_tokens > 0);
  }
  CHECK(std::filesystem::exists(p / ".karkas/mcp/traffic"));
}
