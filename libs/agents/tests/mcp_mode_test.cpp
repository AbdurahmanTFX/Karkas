// Agents in mode "mcp" (docs/adr/0024): the same engine and protocol as with an API; an MCP
// client — here a thread playing Claude Code — is the model and answers every turn through the
// queue (kk_agent_next / kk_agent_act).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "kk/agents/engine.hpp"
#include "kk/agents/launcher.hpp"
#include "kk/agents/mcp_turns.hpp"
#include "kk/llm/mcp_bridge.hpp"
#include "kk/core/paths.hpp"
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
    // As a Claude Code session spends them (ADR 0031): what the turn showed written once to a 1-hour cache,
    // the context before it read again.
    CHECK(run.cache_write_tokens > 0);
    CHECK(run.cache_write_1h_tokens == run.cache_write_tokens);
  }
  CHECK(std::filesystem::exists(p / ".karkas/mcp/traffic"));
}

TEST_CASE("mcp mode: a list wait ends as soon as the tickets this server started have ended (ADR 0029)") {
  test::TempDir d;
  agents::McpTurns turns(d.path());
  std::atomic<int> running{1};
  turns.tickets_running = [&] { return running.load(); };
  turns.tickets_status = [&] { return running ? std::string("T-0001: its agents are working.\n") : std::string("T-0001: ended — done.\n  fact   b done: x\n"); };

  // Still running: the wait runs out (1 s) and says nobody waits.
  auto t0 = std::chrono::steady_clock::now();
  auto text = turns.next("", 1, true);
  CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(900));
  CHECK(core::contains(text, "No agent turn waits."));
  CHECK(core::contains(text, "its agents are working"));

  // Ended while the client waits: the wait of 30 s ends at once, with the result.
  std::thread end_it([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    running = 0;
  });
  t0 = std::chrono::steady_clock::now();
  text = turns.next("", 30, true);
  end_it.join();
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
  CHECK(core::contains(text, "have ended"));
  CHECK(core::contains(text, "T-0001: ended — done."));
  CHECK(core::contains(text, "fact   b done: x"));

  // A server that started nothing keeps waiting for turns of other processes.
  turns.tickets_running = [] { return -1; };
  t0 = std::chrono::steady_clock::now();
  text = turns.next("", 1, true);
  CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(900));
}

TEST_CASE("mcp mode: a wait for a run that has ended returns at once, not when wait_s runs out (ADR 0029)") {
  test::TempDir d;
  std::filesystem::create_directories(d.path() / ".karkas" / "runs");
  REQUIRE(core::write_file(d.path() / ".karkas" / "runs" / "r1.jsonl",
                           R"({"type":"run_start","ticket":"T-0001","role":"worker","block":"b"})" "\n"
                           R"({"type":"run_end","outcome":"done"})" "\n"));
  agents::McpTurns turns(d.path());
  auto t0 = std::chrono::steady_clock::now();
  auto text = turns.next("r1.jsonl", 30, false);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
  CHECK(core::contains(text, "Run r1.jsonl ended: done."));
}

TEST_CASE("mcp mode: with launch = claude Karkas serves a run itself, with a session on the login and never an API key (ADR 0029)") {
  test::FixtureCopy p{"wavkit"};
  test::FakeTests tests;
  auto cfg = test::test_config();
  cfg.agents_mode = "mcp";
  cfg.mcp_wait_s = 60;
  cfg.mcp_launch = "claude";
  // A stand-in for claude: it notes its arguments and environment, then answers the first turn of its run
  // with the orchestrator's escalate through the queue's files, as a session does through kk_agent_act.
  auto fake = p / "fake-claude.sh";
  test::write(fake, R"SH(#!/bin/sh
printf '%s\n' "$@" > "$KK_FAKE_OUT/args"
env > "$KK_FAKE_OUT/env"
run=$(printf '%s' "$2" | sed -n 's/^Serve Karkas run \([^ ]*\) to its end.*/\1/p')
stem=${run%.jsonl}
while [ $# -gt 0 ]; do [ "$1" = "--mcp-config" ] && cfg="$2"; shift; done
turns="$(dirname "$(dirname "$cfg")")/turns"
i=0
while [ ! -f "$turns/$stem-001.req.json" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
printf '%s' '{"text":"","calls":[{"name":"escalate","input":{"note":"served by the stand-in session"}}]}' > "$turns/$stem-001.tmp"
mv "$turns/$stem-001.tmp" "$turns/$stem-001.resp.json"
)SH");
  std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
  cfg.mcp_launch_command = fake.string();
  ::setenv("ANTHROPIC_API_KEY", "sk-test-must-not-leak", 1);
  ::setenv("KK_FAKE_OUT", p.path().c_str(), 1);
  agents::EngineOptions o;  // no providers: the engine's own MCP bridge, and its launcher
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), cfg, o);
  REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
  auto r = (*e)->task("the ring can be reset");
  ::unsetenv("ANTHROPIC_API_KEY");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(test::read(p / ".karkas/tickets/T-0001.ksl"), "served by the stand-in session"));
  auto args = test::read(p / "args");
  CHECK(core::contains(args, "--model\nm-strong\n"));  // the model of the orchestrator's tier
  CHECK(core::contains(args, "--tools\n\n--allowedTools\nmcp__karkas__kk_agent_next\nmcp__karkas__kk_agent_act\n"));
  CHECK(core::contains(args, "--strict-mcp-config"));
  auto env = test::read(p / "env");
  CHECK_FALSE(core::contains(env, "ANTHROPIC_API_KEY"));
  CHECK(core::contains(env, "MCP_TOOL_TIMEOUT=900000"));
  // The run is marked as served by Karkas: a client's kk_agent_next leaves it alone.
  bool marked = false;
  for (const auto& f : std::filesystem::directory_iterator(p / ".karkas/mcp/launch")) marked = marked || f.path().extension() == ".served";
  CHECK(marked);
  auto mcp_json = test::read(p / ".karkas/mcp/launch/mcp.json");
  CHECK(core::contains(mcp_json, "--only-agent"));
}

TEST_CASE("mcp mode: a run Karkas serves itself leaves a nested run to a session of that run's tier (ADR 0029)") {
  test::TempDir d;
  std::filesystem::create_directories(d.path() / ".karkas" / "runs");
  REQUIRE(core::write_file(d.path() / ".karkas" / "runs" / "A.jsonl",
                           R"({"type":"run_start","ticket":"T-0001","role":"orchestrator","block":"root"})" "\n"));
  llm::McpQueue q(d.path() / ".karkas" / "mcp");
  llm::McpTurn nested;
  nested.id = "B-001";
  nested.run = "B.jsonl";
  nested.ticket = "T-0001";
  nested.role = "ask";
  nested.block = "game.player";
  nested.tier = "cheap";
  nested.model = "m-cheap";
  nested.seq = 1;
  nested.ts = core::unix_now();
  REQUIRE(q.put(nested, R"({"id":"B-001","system":"answer","tools":[],"messages":[{"role":"user","content":"Question: why?"}]})"));
  agents::McpTurns turns(d.path());
  // Served by a client: the client of the parent gets the nested turn, as before.
  auto as_client = turns.next("A.jsonl", 1, false);
  CHECK(core::contains(as_client, "Your run A.jsonl waits for this one"));
  // Served by Karkas: the nested turn stays for a session of its own tier.
  std::filesystem::remove(d.path() / ".karkas" / "mcp" / "turns" / "B-001.taken");
  REQUIRE(core::write_file(agents::RunLauncher::marker(d.path(), "A.jsonl"), ""));
  auto launched = turns.next("A.jsonl", 1, false);
  CHECK_FALSE(core::contains(launched, "waits for this one"));
  CHECK(core::contains(launched, "No turn of run A.jsonl waits yet"));
}

TEST_CASE("mcp mode: a session that hits the claude.ai usage limit pauses the launches, the turn waits and the try does not count (ADR 0029)") {
  CHECK(agents::RunLauncher::usage_limit(
            R"J({"type":"result","subtype":"success","is_error":true,"api_error_status":429,"api_error":"usage_limit_reached","result":"You've hit your 5-hour limit · resets 11:50pm (Asia/Bishkek)"})J") ==
        "You've hit your 5-hour limit · resets 11:50pm (Asia/Bishkek)");
  CHECK(agents::RunLauncher::usage_limit(R"({"type":"result","is_error":false,"result":"done"})").empty());
  CHECK(agents::RunLauncher::usage_limit(R"({"type":"result","is_error":true,"api_error_status":"429","result":"overloaded"})").empty());

  test::FixtureCopy p{"wavkit"};
  test::FakeTests tests;
  auto cfg = test::test_config();
  cfg.agents_mode = "mcp";
  cfg.mcp_wait_s = 5;  // past the 3 s before a launch, shorter than the pause: without it the turn would time out
  cfg.mcp_launch = "claude";
  // The first session ends at once on the usage limit, as claude -p does; the next one serves the turn.
  auto fake = p / "fake-claude.sh";
  test::write(fake, R"SH(#!/bin/sh
n=$(cat "$KK_FAKE_OUT/starts" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$KK_FAKE_OUT/starts"
if [ "$n" = 1 ]; then
  echo '{"type":"result","subtype":"success","is_error":true,"api_error_status":429,"api_error":"usage_limit_reached","result":"You'"'"'ve hit your 5-hour limit · resets 11:50pm"}'
  exit 1
fi
run=$(printf '%s' "$2" | sed -n 's/^Serve Karkas run \([^ ]*\) to its end.*/\1/p')
stem=${run%.jsonl}
while [ $# -gt 0 ]; do [ "$1" = "--mcp-config" ] && cfg="$2"; shift; done
turns="$(dirname "$(dirname "$cfg")")/turns"
printf '%s' '{"text":"","calls":[{"name":"escalate","input":{"note":"served after the pause"}}]}' > "$turns/$stem-001.tmp"
mv "$turns/$stem-001.tmp" "$turns/$stem-001.resp.json"
)SH");
  std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
  cfg.mcp_launch_command = fake.string();
  ::setenv("KK_FAKE_OUT", p.path().c_str(), 1);
  const int old_retry = agents::RunLauncher::limit_retry_s;
  agents::RunLauncher::limit_retry_s = 8;
  agents::EngineOptions o;
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), cfg, o);
  REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
  auto t0 = std::chrono::steady_clock::now();
  auto r = (*e)->task("the ring can be reset");
  auto waited = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
  agents::RunLauncher::limit_retry_s = old_retry;
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto ticket = test::read(p / ".karkas/tickets/T-0001.ksl");
  CHECK_MESSAGE(core::contains(ticket, "served after the pause"), ticket);
  CHECK(test::read(p / "starts") == "2\n");  // one failed start, one after the pause: not a third
  CHECK(waited >= 8);                         // it waited for the pause, past mcp_wait_s
  CHECK_FALSE(std::filesystem::exists(p / ".karkas/mcp/paused_until"));
  // The failed start costs nothing and is no row of costs.jsonl.
  CHECK_FALSE(core::contains(core::read_file(p / ".karkas/mcp/launch/costs.jsonl").value_or(""), "\"is_error\":true"));
}

TEST_CASE("mcp mode: a run whose model's safeguards refuse it goes on at once with the next tier's model (ADR 0029)") {
  CHECK_FALSE(agents::RunLauncher::refusal(
                  R"J({"type":"result","is_error":true,"stop_reason":"refusal","result":"API Error: Sonnet 5.5's safeguards flagged this message (https://www.anthropic.com/legal/aup)."})J")
                  .empty());
  CHECK(agents::RunLauncher::refusal(R"({"type":"result","is_error":false,"stop_reason":"end_turn","result":"done"})").empty());

  test::FixtureCopy p{"wavkit"};
  test::FakeTests tests;
  auto cfg = test::test_config();
  cfg.agents_mode = "mcp";
  cfg.mcp_wait_s = 60;
  cfg.mcp_launch = "claude";
  cfg.roles["orchestrator"] = "medium";  // the first run on m-medium, which refuses
  cfg.mcp_maps.roles["orchestrator"] = "medium";
  {
    agents::RunLauncher l(p.path(), cfg);
    CHECK(l.stronger_model("m-cheap") == "m-medium");
    CHECK(l.stronger_model("m-medium") == "m-strong");
    CHECK(l.stronger_model("m-strong").empty());
  }
  auto fake = p / "fake-claude.sh";
  test::write(fake, R"SH(#!/bin/sh
model=$(printf '%s\n' "$@" | sed -n '/^--model$/{n;p;}')
echo "$model" >> "$KK_FAKE_OUT/models"
if [ "$model" = "m-medium" ]; then
  echo '{"type":"result","subtype":"success","is_error":true,"stop_reason":"refusal","total_cost_usd":0.05,"result":"API Error: the safeguards flagged this message"}'
  exit 1
fi
run=$(printf '%s' "$2" | sed -n 's/^Serve Karkas run \([^ ]*\) to its end.*/\1/p')
stem=${run%.jsonl}
while [ $# -gt 0 ]; do [ "$1" = "--mcp-config" ] && cfg="$2"; shift; done
turns="$(dirname "$(dirname "$cfg")")/turns"
i=0
while [ ! -f "$turns/$stem-001.req.json" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
printf '%s' '{"text":"","calls":[{"name":"escalate","input":{"note":"served by the stronger model"}}]}' > "$turns/$stem-001.tmp"
mv "$turns/$stem-001.tmp" "$turns/$stem-001.resp.json"
)SH");
  std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
  cfg.mcp_launch_command = fake.string();
  ::setenv("KK_FAKE_OUT", p.path().c_str(), 1);
  agents::EngineOptions o;
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), cfg, o);
  REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
  auto t0 = std::chrono::steady_clock::now();
  auto r = (*e)->task("the ring can be reset");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));  // at once, not after mcp_wait_s
  CHECK(core::contains(test::read(p / ".karkas/tickets/T-0001.ksl"), "served by the stronger model"));
  CHECK(test::read(p / "models") == "m-medium\nm-strong\n");
  // The refused try's cost is a row of costs.jsonl (a refusal is billed).
  CHECK(core::contains(core::read_file(p / ".karkas/mcp/launch/costs.jsonl").value_or(""), "\"model\":\"m-medium\""));
}
