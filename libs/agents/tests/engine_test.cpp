#include <doctest/doctest.h>

#include "kk/agents/changes.hpp"
#include "kk/agents/engine.hpp"
#include "kk/code/import.hpp"
#include "kk/core/cancel.hpp"
#include "kk/core/text.hpp"
#include "kk/core/tickets.hpp"
#include "kk/core/validate.hpp"
#include "kk/index/state.hpp"
#include "kk/metrics/metrics.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;
using json = nlohmann::json;

namespace {

struct Rig {
  test::FixtureCopy p{"wavkit"};
  test::ScriptedModel model;
  test::FakeTests tests;
  std::unique_ptr<agents::Engine> engine;
  explicit Rig(bool fake_tests = true, core::Config cfg = test::test_config()) {
    agents::EngineOptions o;
    o.providers = model.factory();
    if (fake_tests) o.tests = &tests;
    auto e = agents::Engine::create(p.path(), cfg, o);
    REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
    engine = std::move(*e);
  }
  std::string ticket_text(const std::string& id) { return test::read(p / ".karkas/tickets" / (id + ".ksl")); }
};

json dispatch(const std::string& block, const std::string& difficulty, const std::string& why) {
  return {{"block", block}, {"difficulty", difficulty}, {"why", why}};
}

agents::TestRun failing() {
  agents::TestRun r;
  r.build_ok = true;
  r.ok = false;
  r.text = "tests stream.ring contract 2/3\nfail contract: never loses or duplicates frames\n";
  return r;
}

}  // namespace

TEST_CASE("engine: a bug descends the schema to the leaf and is fixed (13.3, 10.2)") {
  Rig rig;
  rig.model.tool("dispatch", {{"block", "stream"}, {"suspect", {"stream", "dsp"}}, {"difficulty", "M"},
                              {"why", "frames dropped before dsp; ring runs empty"}});
  rig.model.tool("dispatch", {{"block", "stream.ring"}, {"suspect", {"stream.ring", "stream.pump"}},
                              {"ruled_out", {"stream.pump"}}, {"difficulty", "M"}, {"why", "drops at wraparound"}});
  rig.model.tool("read_file", {{"path", "impl/ring.cpp"}});
  rig.model.tool("report", {{"status", "fixed"}, {"cause", "off-by-one at wraparound"}, {"fix", "mask index with cap-1"},
                            {"files", {"impl/ring.cpp"}}, {"api", "unchanged"}});
  auto r = rig.engine->debug("crackle in output after ~10 s of playback", {"contract stream.ring/no_drop FAIL"});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  closed"));
  CHECK(core::contains(t, "L0 orchestrator at=root suspect=stream,dsp pick=stream difficulty=M"));
  CHECK(core::contains(t, "L1 navigator    at=stream suspect=stream.ring,stream.pump ruled_out=stream.pump pick=stream.ring"));
  CHECK(core::contains(t, "L2 worker       at=stream.ring model=medium attempt=1 outcome=fixed"));
  CHECK(core::contains(t, "cause   off-by-one at wraparound"));
  CHECK(core::contains(t, "api     unchanged"));
  CHECK(core::contains(t, "verify"));
  // The program writes the history line of the leaf (3.10).
  CHECK(core::contains(test::read(rig.p / "blocks/stream/ring/block.ksl"), "hist    T-0001 off-by-one at wraparound -> mask index"));
  // Tiers: strong orchestrator, medium navigator, routed worker (est M + thread crossing + difficulty M = 4).
  REQUIRE(rig.model.calls.size() == 4);
  CHECK(rig.model.calls[0].tier == "strong");
  CHECK(rig.model.calls[1].tier == "medium");
  CHECK(rig.model.calls[2].tier == "medium");
  // Nobody above the leaf ever receives code (13.3).
  for (int i = 0; i < 2; ++i) {
    auto text = test::ScriptedModel::request_text(rig.model.calls[static_cast<std::size_t>(i)].req);
    CHECK_FALSE(core::contains(text, "#include"));
    CHECK_FALSE(core::contains(text, "std::atomic"));
  }
  auto worker_text = test::ScriptedModel::request_text(rig.model.calls[2].req);
  CHECK(core::contains(worker_text, "std::atomic"));
  // The worker gets the path view: the L0 suspect dsp (another branch) is not in it (app. D).
  CHECK(core::contains(worker_text, "@ticket T-0001 view=path at=stream.ring"));
  CHECK_FALSE(core::contains(worker_text, "suspect=stream,dsp"));
  // Runs reach the index from the journals.
  CHECK(rig.engine->index().runs().size() == 3);
}

TEST_CASE("engine: failing tests escalate the worker cheap -> medium -> strong -> human (11.3)") {
  Rig rig;
  rig.tests.fn = [](const std::vector<std::string>&) { return failing(); };
  rig.model.tool("dispatch", dispatch("stream", "S", "frames dropped"));
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring drops"));
  for (int i = 0; i < 6; ++i) rig.model.tool("report", {{"status", "fixed"}, {"cause", "maybe"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE(r);
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  escalated"));
  std::vector<std::string> tiers;
  for (std::size_t i = 2; i < rig.model.calls.size(); ++i) tiers.push_back(rig.model.calls[i].tier);
  CHECK(tiers == std::vector<std::string>{"cheap", "cheap", "medium", "medium", "strong", "strong"});
  CHECK(core::contains(t, "attempt=6 outcome=failed"));
  // The next model gets a summary of the previous attempts.
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[4].req), "previous attempts"));
}

TEST_CASE("engine: not_here sends the ticket back to the navigator, which picks another child") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "frames dropped before dsp"));
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring suspected"));
  rig.model.tool("report", {{"status", "not_here"}, {"cause", "ring keeps every frame; pump pushes short chunks"}});
  rig.model.tool("dispatch", {{"block", "stream.pump"}, {"ruled_out", {"stream.ring"}}, {"difficulty", "S"}, {"why", "ring ruled out"}});
  rig.model.tool("report", {{"status", "fixed"}, {"cause", "partial push ignored"}, {"fix", "retry until accepted"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE(r);
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  closed"));
  CHECK(core::contains(t, "at=stream.ring model=cheap attempt=1 outcome=not_here"));
  CHECK(core::contains(t, "fact    stream.ring not_here: ring keeps every frame; pump pushes short chunks"));
  CHECK(core::contains(t, "ruled_out=stream.ring pick=stream.pump"));
}

TEST_CASE("engine: needs_contract_change becomes a pending @change and the ticket waits for a human") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "x"));
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "y"));
  rig.model.tool("report", {{"status", "needs_contract_change"}, {"cause", "consumer needs to drain the ring"},
                            {"api", "change_requested"}, {"proposal", "~ @block stream.ring\n+ fn reset() -> void"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE(r);
  CHECK(core::contains(rig.ticket_text("T-0001"), "status  escalated"));
  agents::ChangeStore changes(rig.p.path());
  auto c = changes.load("C-0001");
  REQUIRE(c);
  CHECK(c->status.value == "pending");
  CHECK(c->from.value == "worker T-0001");
  REQUIRE(c->diff.size() == 2);
  CHECK(c->diff[1].text == "fn reset() -> void");
  // Approval applies the diff, unfreezes nothing (not frozen) and notes the ticket.
  auto a = rig.engine->approve("C-0001");
  REQUIRE_MESSAGE(a, (a ? "" : a.error().str()));
  CHECK(core::contains(test::read(rig.p / "blocks/stream/ring/block.ksl"), "fn      reset() -> void"));
  CHECK(core::contains(rig.ticket_text("T-0001"), "note    C-0001 approved"));
}

TEST_CASE("engine: a code diff of api/ as proposal is refused by report and the worker corrects it in the same run") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "x"));
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "y"));
  // First a unified diff of api/ instead of a KSL card diff: report refuses it (not terminal).
  rig.model.tool("report", {{"status", "needs_contract_change"},
                            {"cause", "api/ring.hpp does not declare reset() though the card does"},
                            {"api", "change_requested"},
                            {"proposal", "--- api/ring.hpp\n+++ api/ring.hpp\n void push();\n+void reset();"}});
  // Then, as the error says, no proposal and the missing declaration in cause and fix.
  rig.model.tool("report", {{"status", "needs_contract_change"},
                            {"cause", "api/ring.hpp does not declare reset() though the card does"},
                            {"fix", "Add void reset(); to api/ring.hpp"},
                            {"api", "change_requested"}});
  // Up to the orchestrator (11.3), which sees the worker's cause and gives the ticket to the human.
  rig.model.tool("escalate", {{"note", "add void reset(); to api/ring.hpp, then karkas approve api stream.ring"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 5);  // orchestrator, navigator, two worker steps, orchestrator
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[3].req), "proposal cannot become a @change"));
  auto orch = test::ScriptedModel::request_text(rig.model.calls[4].req);
  CHECK(rig.model.calls[4].tier == "strong");
  CHECK(core::contains(orch, "Add void reset(); to api/ring.hpp"));
  CHECK(core::contains(orch, "stream.ring: needs_contract_change"));
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  escalated"));
  CHECK(core::contains(t, "attempt=1 outcome=needs_contract_change"));
  CHECK(core::contains(t, "L0 orchestrator at=root outcome=escalated"));
  CHECK(core::contains(t, "note    add void reset(); to api/ring.hpp, then karkas approve api stream.ring"));
  // No @change is left behind.
  agents::ChangeStore changes(rig.p.path());
  auto cs = changes.list();
  REQUIRE(cs);
  CHECK(cs->empty());
}

TEST_CASE("engine: a KSL proposal that changes no card is refused by report and the worker corrects it in the same run") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "x"));
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "y"));
  // Parses as KSL, but only selects a block: report refuses it (not terminal).
  rig.model.tool("report", {{"status", "needs_contract_change"}, {"cause", "ring needs reset"},
                            {"api", "change_requested"}, {"proposal", "~ @block stream.ring"}});
  rig.model.tool("report", {{"status", "needs_contract_change"}, {"cause", "ring needs reset"},
                            {"api", "change_requested"}, {"proposal", "~ @block stream.ring\n+ fn reset() -> void"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 4);
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[3].req), "the diff changes no card"));
  agents::ChangeStore changes(rig.p.path());
  auto cs = changes.list();
  REQUIRE(cs);
  CHECK(cs->size() == 1);
}

TEST_CASE("engine: '~' before a card line is refused by report with the syntax of a line change") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "x"));
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "y"));
  rig.model.tool("report", {{"status", "needs_contract_change"}, {"cause", "ring needs reset"},
                            {"api", "change_requested"}, {"proposal", "~ @block stream.ring\n~ purpose a ring\n+ fn reset() -> void"}});
  rig.model.tool("report", {{"status", "needs_contract_change"}, {"cause", "ring needs reset"},
                            {"api", "change_requested"}, {"proposal", "~ @block stream.ring\n+ fn reset() -> void"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 4);
  auto err = test::ScriptedModel::request_text(rig.model.calls[3].req);
  CHECK(core::contains(err, "'~' selects a record"));
  CHECK(core::contains(err, "'- old line' then '+ new line'"));
  agents::ChangeStore changes(rig.p.path());
  auto cs = changes.list();
  REQUIRE(cs);
  CHECK(cs->size() == 1);
}

TEST_CASE("engine: needs_contract_change with no proposal goes up to the orchestrator, which cannot send it back") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "x"));
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "y"));
  rig.model.tool("report", {{"status", "needs_contract_change"}, {"cause", "the card is right but api/ is stale"},
                            {"fix", "Add void reset(); to api/ring.hpp"}, {"api", "change_requested"}});
  // The orchestrator tries the same leaf again: refused in this run (the leaf already answered).
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "implement reset"));
  rig.model.tool("escalate", {{"note", "api/ring.hpp lacks reset(); add it and approve the api"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 5);
  auto refused = test::ScriptedModel::request_text(rig.model.calls[4].req);
  CHECK(core::contains(refused, "stream.ring already answered in this run (needs_contract_change"));
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  escalated"));
  CHECK(core::contains(t, "fact    stream.ring needs_contract_change: the card is right but api/ is stale; Add void reset();"));
  CHECK(core::contains(t, "api/ring.hpp lacks reset(); add it and approve the api"));
  CHECK(core::contains(t, "state   "));
  agents::ChangeStore changes(rig.p.path());
  auto cs = changes.list();
  REQUIRE(cs);
  CHECK(cs->empty());
  // Running the ticket again with nothing changed starts no model (ADR 0020)...
  auto calls = rig.model.calls.size();
  auto again = rig.engine->run_ticket("T-0001");
  REQUIRE_FALSE(again);
  CHECK(core::contains(again.error().message, "nothing changed since"));
  CHECK(core::contains(again.error().message, "--force"));
  CHECK(rig.model.calls.size() == calls);
  // ...a human change of the api/ unblocks it.
  auto api = rig.p / "blocks/stream/ring/api/ring.hpp";
  test::write(api, test::read(api) + "\n// reset() added by the human\n");
  rig.model.tool("close", {{"verdict", "done"}, {"note", "api updated by the human"}});
  auto after = rig.engine->run_ticket("T-0001");
  REQUIRE_MESSAGE(after, (after ? "" : after.error().str()));
  CHECK(rig.model.calls.size() == calls + 1);
  // The orchestrator is told that the project changed since the last result.
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls.back().req), "changed since the last result"));
}

TEST_CASE("engine: models pinned per level and per block override the roles (ADR 0011)") {
  auto cfg = test::test_config();
  cfg.levels = {{0, "medium"}, {2, "strong"}};
  cfg.block_tiers = {{"stream.ring", "cheap"}};
  Rig rig(true, cfg);
  rig.model.tool("dispatch", dispatch("stream", "M", "x"));
  rig.model.tool("ask", {{"block", "stream.pump"}, {"question", "rate?"}});
  rig.model.tool("answer", {{"text", "48k"}});
  rig.model.tool("dispatch", dispatch("stream.ring", "L", "ring"));
  rig.model.tool("report", {{"status", "fixed"}, {"cause", "c"}, {"fix", "f"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() >= 5);
  CHECK(rig.model.calls[0].tier == "medium");  // orchestrator: level 0
  CHECK(rig.model.calls[1].tier == "medium");  // navigator of stream: level 1 not pinned, role tier
  CHECK(rig.model.calls[2].tier == "strong");  // ask stream.pump: level 2
  CHECK(rig.model.calls[4].tier == "cheap");   // worker stream.ring: its block beats level 2 and routing (L)
  CHECK(core::contains(rig.ticket_text("T-0001"), "model=cheap"));
}

TEST_CASE("engine: ask() runs a short agent of the child and appends only its answer (10.5)") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "x"));
  rig.model.tool("ask", {{"block", "stream.pump"}, {"question", "at what rate does the pump push?"}});
  rig.model.tool("answer", {{"text", "chunks of 256 frames, about 48k frames/s"}});
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "pump rate ok"));
  rig.model.tool("report", {{"status", "fixed"}, {"cause", "c"}, {"fix", "f"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE(r);
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "answer  at=stream.pump \"chunks of 256 frames, about 48k frames/s\""));
  CHECK(rig.model.calls[2].tier == "cheap");
  // The navigator gets the answer as the tool result, not the pump's context.
  auto nav = test::ScriptedModel::request_text(rig.model.calls[3].req);
  CHECK_FALSE(core::contains(nav, "#include"));
}

TEST_CASE("engine: the tester writes contract tests without reading impl/ (stage 6)") {
  Rig rig;
  rig.model.tool("read_file", {{"path", "impl/ring.cpp"}});
  rig.model.tool("write_file", {{"path", "tests/contract/extra_contract.cpp"},
                                {"content", "#include <doctest/doctest.h>\n#include \"ring.hpp\"\nTEST_CASE(\"x\") {}\n"}});
  rig.model.tool("report", {{"status", "done"}, {"tests", "3 invariants covered"}});
  auto r = rig.engine->run_tester("stream.ring");
  CHECK(r.status == "done");
  CHECK(std::filesystem::exists(rig.p / "blocks/stream/ring/tests/contract/extra_contract.cpp"));
  for (const auto& path : r.reads) CHECK_FALSE(core::contains(path, "impl/"));
  // The attempt to read impl/ was answered with an error.
  const auto& second = rig.model.calls[1].req;
  const auto& results = second.messages.back().content;
  REQUIRE_FALSE(results.empty());
  CHECK(results.back().is_error);
  CHECK(core::contains(results.back().text, "impl/ is not readable"));
  // And its context has no implementation either.
  CHECK_FALSE(core::contains(test::ScriptedModel::request_text(rig.model.calls[0].req), "std::atomic"));
}

TEST_CASE("engine: design top-down with stops for approval (13.1)") {
  test::TempDir d{"design"};
  test::write(d / "project.ksl", "@project calc\nlang    cpp\nstd     c++20\ntest    doctest\n");
  test::ScriptedModel model;
  test::FakeTests tests;
  agents::EngineOptions o;
  o.providers = model.factory();
  o.tests = &tests;
  auto e = agents::Engine::create(d.path(), test::test_config(), o);
  REQUIRE(e);
  auto& engine = **e;
  model.tool("write_card", {{"block", "app"}, {"ksl", "@block app\npurpose command line\nkind    entry\nthread  main\n"}});
  model.tool("write_card", {{"block", "core"}, {"ksl", "@block core\npurpose arithmetic on numbers\nkind    group\n"}});
  model.tool("write_card", {{"block", "core.calc"}, {"ksl", "@block core.calc\npurpose x\nkind    leaf\n"}});  // outside scope
  model.tool("report", {{"status", "done"}, {"summary", "two blocks"}});
  auto r = engine.design("calculator on the command line");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(*r, "app   entry  command line"));
  CHECK(core::contains(*r, "STOP"));
  CHECK_FALSE(std::filesystem::exists(d / "blocks/core/calc/block.ksl"));
  CHECK(core::contains(test::read(d / "project.ksl"), "idea    calculator on the command line"));
  REQUIRE(engine.approve("L0"));

  model.tool("write_card", {{"block", "core.calc"},
                            {"ksl", "@block core.calc\npurpose add two integers\nkind    leaf\nthread  main\nest     S\n"
                                    "fn      add(int, int) -> int\ninv     add is commutative\n"}});
  model.tool("write_api", {{"block", "core.calc"}, {"file", "calc.hpp"},
                           {"code", "#pragma once\nstruct ICalc { virtual ~ICalc() = default; virtual int add(int a, int b) = 0; };\n"}});
  model.tool("write_api", {{"block", "core.calc"}, {"file", "../evil.hpp"}, {"code", "x"}});
  model.tool("report", {{"status", "done"}});
  auto x = engine.expand("core", false);
  REQUIRE_MESSAGE(x, (x ? "" : x.error().str()));
  CHECK(std::filesystem::exists(d / "blocks/core/calc/api/calc.hpp"));
  CHECK_FALSE(std::filesystem::exists(d / "blocks/core/evil.hpp"));
  auto a = engine.approve("api");
  REQUIRE_MESSAGE(a, (a ? "" : a.error().str()));
  CHECK(core::contains(*a, "api frozen for 1 blocks"));
  CHECK(std::filesystem::exists(d / "CMakeLists.txt"));
  CHECK(std::filesystem::exists(d / "blocks/core/calc/impl/calc.cpp"));
  CHECK(core::contains(test::read(d / ".karkas/approvals.json"), "core.calc"));
  // The frozen api cannot be rewritten by the architect.
  model.tool("write_api", {{"block", "core.calc"}, {"file", "calc.hpp"}, {"code", "changed"}});
  model.tool("report", {{"status", "done"}});
  REQUIRE(engine.expand("core.calc", false));
  CHECK_FALSE(core::contains(test::read(d / "blocks/core/calc/api/calc.hpp"), "changed"));
}

TEST_CASE("engine: real build — the ring wraparound bug is found by descent and fixed (stage 6/7 acceptance)") {
  Rig rig(false);
  auto ring = rig.p / "blocks/stream/ring/impl/ring.cpp";
  auto text = test::read(ring);
  REQUIRE(core::contains(text, "buf_[(head + i) & mask_] = frames[i];"));
  test::write(ring, core::replace_all(text, "buf_[(head + i) & mask_] = frames[i];", "buf_[(head + i) % (buf_.size() - 1)] = frames[i];"));
  rig.model.tool("dispatch", dispatch("stream", "M", "frames lost before dsp"));
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "drops at wraparound"));
  rig.model.tool("test", json::object());
  rig.model.tool("edit_file", {{"path", "impl/ring.cpp"},
                               {"old", "buf_[(head + i) % (buf_.size() - 1)] = frames[i];"},
                               {"new", "buf_[(head + i) & mask_] = frames[i];"}});
  rig.model.tool("report", {{"status", "fixed"}, {"cause", "write index wrapped at capacity-1"}, {"fix", "mask the index with cap-1"}});
  auto r = rig.engine->debug("crackle in output", {"contract stream.ring FAIL"});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto t = rig.ticket_text("T-0001");
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
  // The worker saw the failing contract test before fixing it.
  auto after_test = test::ScriptedModel::request_text(rig.model.calls[3].req);
  CHECK(core::contains(after_test, "never loses or duplicates frames"));
  auto snap = rig.engine->index().snapshot();
  CHECK(snap.block("stream.ring")->status == "passing");
}

TEST_CASE("engine: approving an import writes the cards, and a worker works inside an imported block (ADR 0027)") {
  test::FixtureCopy p{"legacy"};
  test::ScriptedModel model;
  test::FakeTests tests;
  agents::EngineOptions o;
  o.providers = model.factory();
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), test::test_config(), o);
  REQUIRE(e);
  auto plan = code::plan_import(p.path());
  REQUIRE(plan);
  auto c = agents::ChangeStore(p.path()).create("import", "karkas import", plan->diff);
  REQUIRE(c);
  auto r = (*e)->approve(c->id);
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(*r, "C-0001 approved: 8 card(s) written"));
  CHECK(core::contains(*r, "describe existing code"));
  CHECK_FALSE(core::contains(*r, "karkas expand"));  // the code exists, no api/ to write
  // The worker edits the files of its card where they are, the project's build checks it.
  model.tool("edit_file", {{"path", "src/game/player.cpp"}, {"old", "pos_.x + dt"}, {"new", "pos_.x + 2 * dt"}});
  model.tool("report", {{"status", "done"}, {"fix", "the player moves twice as fast"}, {"files", {"src/game/player.cpp"}}, {"api", "unchanged"}});
  auto w = (*e)->run_worker("game.player", "", "make the player faster");
  CHECK_MESSAGE(w.status == "done", w.cause);
  CHECK(core::contains(test::read(p / "src/game/player.cpp"), "pos_.x + 2 * dt"));
  CHECK(w.api_note.empty());
  REQUIRE(model.calls.size() == 2);
  auto first = test::ScriptedModel::request_text(model.calls[0].req);
  CHECK(core::contains(first, "WORKER of one block of an imported project"));
  CHECK(core::contains(first, "=== src/game/hpp/player.hpp\n"));  // its header, whole
  CHECK(core::contains(first, "=== src/game/player.cpp\n"));      // its source, whole (small)
  CHECK(core::contains(first, "src/game/hpp/world.hpp"));         // the header of the linked block
  CHECK(core::contains(first, "IsWalkable"));
  CHECK(core::contains(first, "their public part is the contract"));
  CHECK_FALSE(core::contains(first, "tests/unit"));
  // The tester has no Karkas contract tests to write there.
  auto t = (*e)->run_tester("game.player");
  CHECK(t.status == "failed");
  CHECK(core::contains(t.cause, "project's own build and tests"));
}

TEST_CASE("engine: approving the api confirms blocks marked needs_review, also the entry without an api") {
  Rig rig;
  index::StateStore store(rig.p.path());
  REQUIRE(store.mark_review("app", "contract of stream.ring changed"));
  REQUIRE(store.mark_review("stream.pump", "contract of stream.ring changed"));
  auto r = rig.engine->approve("api");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto st = store.load();
  REQUIRE(st);
  CHECK_FALSE(st->review.contains("app"));
  CHECK_FALSE(st->review.contains("stream.pump"));
}

TEST_CASE("engine: approve api in a project with its own CMakeLists.txt freezes and generates no build (ADR 0016)") {
  Rig rig;
  const std::string own = "cmake_minimum_required(VERSION 3.20)\nproject(wavkit CXX)\n# the project's own build\n";
  test::write(rig.p / "CMakeLists.txt", own);
  auto r = rig.engine->approve("api");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(*r, "api frozen for"));
  CHECK(core::contains(*r, "own CMakeLists.txt"));
  CHECK(test::read(rig.p / "CMakeLists.txt") == own);
}

TEST_CASE("engine: a worker stopped by the user ends the descent, the ticket stays open, nothing escalates (ADR 0019)") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "frames dropped"));
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "ring drops"));
  // While the worker reads a file, the user presses "Stop" on it in the GUI.
  rig.model.then([&](const llm::Request&) {
    for (const auto& a : metrics::active_runs(rig.p.path())) {
      if (a.role == "worker") REQUIRE(metrics::request_stop(rig.p.path(), a.journal));
    }
    return test::tool_response("read_file", {{"path", "impl/ring.cpp"}});
  });
  for (int i = 0; i < 6; ++i) rig.model.tool("report", {{"status", "fixed"}, {"cause", "never reached"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "cancelled");
  CHECK(core::contains(r.error().message, "karkas ticket run T-0001"));
  CHECK(rig.model.calls.size() == 3);  // no second attempt, no stronger tier
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  open"));
  CHECK(core::contains(t, "at=stream.ring model=medium attempt=1 outcome=cancelled"));
  CHECK(core::contains(t, "stopped by the user at stream.ring (worker)"));
  CHECK(metrics::active_runs(rig.p.path()).empty());
  bool journal_says_cancelled = false;
  for (const auto& run : rig.engine->index().runs()) journal_says_cancelled |= run.role == "worker" && run.outcome == "cancelled";
  CHECK(journal_says_cancelled);
}

TEST_CASE("engine: SIGINT of the command stops the navigator and the descent (ADR 0019)") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "frames dropped"));
  rig.model.then([&](const llm::Request&) {
    core::request_cancel();  // what the CLI's signal handler does
    return test::tool_response("view", {{"block", "stream"}});
  });
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "never reached"));
  auto r = rig.engine->debug("crackle", {});
  core::reset_cancel();
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "cancelled");
  CHECK(rig.model.calls.size() == 2);
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  open"));
  CHECK(core::contains(t, "stopped by the user at stream (navigator)"));
}

// --- ADR 0020: reasonable talk between the agents, no circles ---------------------------------

TEST_CASE("engine: a feature over two leaves comes back to the orchestrator after each done leaf") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring part"));  // a known leaf: no navigator hop
  rig.model.tool("report", {{"status", "done"}, {"fix", "ring gains reset"}, {"files", {"impl/ring.cpp"}}});
  rig.model.tool("dispatch", dispatch("stream.pump", "S", "pump part"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "pump calls reset on stop"}});
  rig.model.tool("close", {{"verdict", "done"}, {"note", "both parts in place"}});
  auto r = rig.engine->task("reset the stream on stop");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 5);
  auto second = test::ScriptedModel::request_text(rig.model.calls[2].req);
  CHECK(core::contains(second, "Finished in this run: stream.ring"));
  CHECK(core::contains(second, "stream.ring: done: ring gains reset"));
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  closed"));
  CHECK(core::contains(t, "status  done\nfix     stream.ring: ring gains reset\nfiles   stream.ring: impl/ring.cpp"));
  CHECK(core::contains(t, "fix     stream.pump: pump calls reset on stop"));
  CHECK(core::contains(t, "note    both parts in place"));
}

TEST_CASE("engine: a done leaf of a feature is not sent the ticket again in the same run") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring part"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "ring gains reset"}});
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring again"));
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("reset the stream on stop");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 4);
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[3].req), "stream.ring already answered in this run (done"));
}

TEST_CASE("engine: a cause in a contract test goes to the tester (needs_tester), who corrects it") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "contract test flakes"));
  rig.model.tool("report", {{"status", "needs_tester"}, {"cause", "the contract test samples fill before the last push"},
                            {"fix", "sample fill after the last push"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "the test samples fill after the last push"},
                            {"files", {"tests/contract/ring_contract.cpp"}}});
  auto r = rig.engine->debug("ring contract test flakes", {});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 3);
  CHECK(rig.model.calls[2].tier == "cheap");  // the tester's tier of [roles]
  auto tester = test::ScriptedModel::request_text(rig.model.calls[2].req);
  CHECK(core::contains(tester, "You are the TESTER"));
  CHECK(core::contains(tester, "the contract test samples fill before the last push"));
  CHECK(core::contains(tester, "Never weaken or drop an invariant"));
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  closed"));
  CHECK(core::contains(t, "outcome=needs_tester"));
  CHECK(core::contains(t, "L2 tester       at=stream.ring model=cheap attempt=1 outcome=done"));
  CHECK(core::contains(t, "status  fixed"));
  CHECK(core::contains(t, "files   tests/contract/ring_contract.cpp"));
}

TEST_CASE("engine: in a feature, a leaf done after its tester fixed a contract test goes back to the orchestrator") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring part"));
  rig.model.tool("report", {{"status", "needs_tester"}, {"cause", "reset is in place; the contract test assumes no reset"},
                            {"fix", "let the test reset the ring first"}, {"files", {"impl/ring.cpp"}}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "the test resets the ring first"}, {"files", {"tests/contract/ring_contract.cpp"}}});
  rig.model.tool("dispatch", dispatch("stream.pump", "S", "pump part"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "pump calls reset on stop"}});
  rig.model.tool("close", {{"verdict", "done"}, {"note", "both parts in place"}});
  auto r = rig.engine->task("reset the stream on stop");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 6);  // flappy T-0014 closed after the tester, its second block undone
  auto orch = test::ScriptedModel::request_text(rig.model.calls[3].req);
  CHECK(core::contains(orch, "stream.ring: done (after the tester fixed a contract test)"));
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  closed"));
  CHECK(core::contains(t, "fix     stream.ring: the test resets the ring first"));
  CHECK(core::contains(t, "files   stream.ring: impl/ring.cpp, tests/contract/ring_contract.cpp"));
  CHECK(core::contains(t, "fix     stream.pump: pump calls reset on stop"));
}

TEST_CASE("engine: a closed ticket runs again only with force, which reopens it") {
  Rig rig;
  rig.model.tool("close", {{"verdict", "done"}, {"note", "nothing to do"}});
  REQUIRE(rig.engine->task("tidy the ring"));
  auto again = rig.engine->run_ticket("T-0001");
  REQUIRE(again);
  CHECK(core::contains(*again, "is closed (karkas ticket run T-0001 --force reopens it)"));
  CHECK(rig.model.calls.size() == 1);
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "the part left undone"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "ring tidied"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto forced = rig.engine->run_ticket("T-0001", true);
  REQUIRE_MESSAGE(forced, (forced ? "" : forced.error().str()));
  CHECK(rig.model.calls.size() == 4);
  CHECK(core::contains(rig.ticket_text("T-0001"), "fix     stream.ring: ring tidied"));
}

TEST_CASE("engine: a navigator cannot send the ticket back to a child that just said not_here") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream", "M", "frames dropped"));
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring suspected"));
  rig.model.tool("report", {{"status", "not_here"}, {"cause", "the flaw is in the pump"}});
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring again"));  // refused
  rig.model.tool("dispatch", dispatch("stream.pump", "S", "pump then"));
  rig.model.tool("report", {{"status", "fixed"}, {"cause", "short pushes ignored"}, {"fix", "retry"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 6);
  auto nav = test::ScriptedModel::request_text(rig.model.calls[3].req);
  CHECK(core::contains(nav, "Already answered in this run"));
  CHECK(core::contains(nav, "tried   stream.ring -> worker at stream.ring: not_here | stream.ring not_here: the flaw is in the pump"));
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[4].req), "stream.ring already answered in this run (not_here"));
  CHECK(core::contains(rig.ticket_text("T-0001"), "status  closed"));
}

TEST_CASE("engine: a ticket whose @change waits for approval is not run again") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream.ring", "M", "y"));
  rig.model.tool("report", {{"status", "needs_contract_change"}, {"cause", "consumer needs to drain the ring"},
                            {"proposal", "~ @block stream.ring\n+ fn reset() -> void"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE(r);
  auto calls = rig.model.calls.size();
  auto again = rig.engine->run_ticket("T-0001");
  REQUIRE_FALSE(again);
  CHECK(core::contains(again.error().message, "T-0001 waits for C-0001"));
  CHECK(core::contains(again.error().message, "karkas approve C-0001"));
  CHECK(rig.model.calls.size() == calls);
}

TEST_CASE("engine: [routing] escalate moves a failing worker along a chain of any tier names") {
  auto cfg = test::test_config();
  cfg.models["ds_flash"] = cfg.models["cheap"];
  cfg.models["ds_pro"] = cfg.models["strong"];
  cfg.block_tiers = {{"stream.ring", "ds_flash"}};
  cfg.escalate = {"ds_flash", "ds_pro"};
  Rig rig(true, cfg);
  rig.tests.fn = [](const std::vector<std::string>&) { return failing(); };
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring drops"));
  for (int i = 0; i < 4; ++i) rig.model.tool("report", {{"status", "fixed"}, {"cause", "maybe"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE(r);
  std::vector<std::string> tiers;
  for (std::size_t i = 1; i < rig.model.calls.size(); ++i) tiers.push_back(rig.model.calls[i].tier);
  CHECK(tiers == std::vector<std::string>{"ds_flash", "ds_flash", "ds_pro", "ds_pro"});
  CHECK(core::contains(rig.ticket_text("T-0001"), "status  escalated"));
  CHECK(agents::next_tier(cfg, "ds_pro").empty());
  CHECK(agents::next_tier(test::test_config(), "cheap") == "medium");
}

TEST_CASE("engine: in a ticket the worker brings an unfrozen api/ in line with the card") {
  Rig rig;
  index::StateStore st(rig.p.path());
  REQUIRE(st.freeze_api("stream.pump", "x"));  // the project went through approve api; ring was unfrozen
  // The approved change added reset() to the card: api/ is behind it (D04).
  auto card = rig.p / "blocks/stream/ring/block.ksl";
  auto text = test::read(card);
  text.insert(text.find("\nfn      capacity()"), "\nfn      reset()");
  test::write(card, text);
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "card gained reset"));
  rig.model.tool("edit_file", {{"path", "api/ring.hpp"},
                               {"old", "  [[nodiscard]] virtual std::size_t fill() const = 0;"},
                               {"new", "  [[nodiscard]] virtual std::size_t fill() const = 0;\n  virtual void reset() = 0;"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "reset() declared and implemented"}, {"api", "changed"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("the ring can be reset");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[1].req), "unfrozen and behind its card"));
  CHECK(core::contains(test::read(rig.p / "blocks/stream/ring/api/ring.hpp"), "virtual void reset() = 0;"));
  // Without auto_approve the freeze is the human's: the note says so and names no step for a client.
  CHECK(core::contains(rig.ticket_text("T-0001"),
                       "api     stream.ring: changed to the card; a human freezes it with `karkas approve api stream.ring`"));
  auto frozen = index::StateStore(rig.p.path()).load();
  REQUIRE(frozen);
  CHECK_FALSE(frozen->frozen_api.contains("stream.ring"));
}

TEST_CASE("engine: with auto_approve the api a worker brought in line with an approved card is frozen by the program (ADR 0029)") {
  auto cfg = test::test_config();
  cfg.auto_approve = true;
  Rig rig(true, cfg);
  REQUIRE(index::StateStore(rig.p.path()).freeze_api("stream.pump", "x"));  // went through approve api; ring was unfrozen
  auto card = rig.p / "blocks/stream/ring/block.ksl";
  auto text = test::read(card);
  text.insert(text.find("\nfn      capacity()"), "\nfn      reset()");
  test::write(card, text);
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "card gained reset"));
  rig.model.tool("edit_file", {{"path", "api/ring.hpp"},
                               {"old", "  [[nodiscard]] virtual std::size_t fill() const = 0;"},
                               {"new", "  [[nodiscard]] virtual std::size_t fill() const = 0;\n  virtual void reset() = 0;"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "reset() declared and implemented"}, {"api", "changed"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("the ring can be reset");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto ticket = rig.ticket_text("T-0001");
  CHECK(core::contains(ticket, "api     stream.ring: changed to the card and frozen (auto_approve)"));
  CHECK_FALSE(core::contains(ticket, "karkas approve api"));
  auto st = index::StateStore(rig.p.path()).load();
  REQUIRE(st);
  REQUIRE(st->frozen_api.contains("stream.ring"));
  auto s = core::load_schema(rig.p.path());
  REQUIRE(s);
  CHECK(st->frozen_api.at("stream.ring") == index::block_api_hash(index::collect_code_files(*s), "stream.ring"));
}

TEST_CASE("engine: a worker tunes a constant of a frozen api/, which stays frozen with the new hash") {
  Rig rig;
  test::write(rig.p / "blocks/stream/ring/api/limits.hpp", "#pragma once\nnamespace wk {\ninline constexpr int kMaxFrames = 4096;\n}\n");
  auto hash = [&] {
    auto s = core::load_schema(rig.p.path());
    REQUIRE(s);
    return index::block_api_hash(index::collect_code_files(*s), "stream.ring");
  };
  index::StateStore st(rig.p.path());
  REQUIRE(st.freeze_api("stream.ring", hash()));
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "a bigger ring"));
  rig.model.tool("edit_file", {{"path", "api/limits.hpp"}, {"old", "4096"}, {"new", "8192"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "kMaxFrames 4096 -> 8192"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("allow 8192 frames");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[1].req), "frozen: its declarations"));
  CHECK(core::contains(test::read(rig.p / "blocks/stream/ring/api/limits.hpp"), "8192"));
  auto state = st.load();
  REQUIRE(state);
  CHECK(state->frozen_api.at("stream.ring") == hash());  // no D05: the frozen hash follows the values
  // The verification re-tests the dependents of the edited api/ (12.4).
  bool dependents = false;
  for (const auto& c : rig.tests.calls) dependents = dependents || std::find(c.begin(), c.end(), "dsp.chain") != c.end();
  CHECK(dependents);
  CHECK(core::contains(rig.ticket_text("T-0001"), "constants tuned or added, declarations unchanged (re-frozen)"));
}

TEST_CASE("engine: the orchestrator's change that changes no card is refused by the tool and corrected") {
  Rig rig;
  rig.model.tool("propose_change", {{"reason", "ring needs reset"}, {"diff", "~ @block stream.ring"}});
  rig.model.tool("propose_change", {{"reason", "ring needs reset"}, {"diff", "~ @block stream.ring\n+ fn reset() -> void"}});
  auto r = rig.engine->task("the ring can be reset");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 2);
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[1].req), "the change cannot be proposed"));
  CHECK(core::contains(rig.ticket_text("T-0001"), "C-0001 needs human approval"));
}

TEST_CASE("engine: ask() cuts a long answer on a word boundary, never inside a character") {
  Rig rig;
  std::string longer;
  for (int i = 0; i < 80; ++i) longer += "слово ";
  rig.model.tool("answer", {{"text", longer}});
  auto a = rig.engine->ask("stream.pump", "rate?", "");
  REQUIRE(a);
  CHECK(a->ends_with("слово..."));
  CHECK(core::estimate_tokens(*a) <= 82);
}

TEST_CASE("engine: the schema agents get no probes tool without probe data, the journal keeps what the model saw") {
  Rig rig;
  rig.model.tool("close", {{"verdict", "not_a_bug"}});
  auto r = rig.engine->task("nothing");
  REQUIRE(r);
  bool probes = false;
  for (const auto& t : rig.model.calls[0].req.tools) probes = probes || t.name == "probes";
  CHECK_FALSE(probes);
  auto runs = rig.engine->index().runs();
  REQUIRE_FALSE(runs.empty());
  bool found = false;
  for (const auto& e : std::filesystem::directory_iterator(rig.p / ".karkas/runs")) {
    if (!e.is_regular_file()) continue;
    auto text = test::read(e.path());
    if (core::contains(text, "\"kind\":\"start\"") && core::contains(text, "You are the ORCHESTRATOR") &&
        core::contains(text, "\"context\":\"idea ")) {
      found = true;
    }
  }
  CHECK(found);
}

TEST_CASE("engine: [effort] per role reaches the request, other roles keep the tier's effort") {
  auto cfg = test::test_config();
  cfg.role_effort = {{"orchestrator", "low"}};
  cfg.models["medium"].effort = "high";
  CHECK(cfg.max_output_tokens_routing == 0);  // off by default
  cfg.max_output_tokens_routing = 2048;
  Rig rig(true, cfg);
  rig.model.tool("dispatch", dispatch("stream", "S", "x"));
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "y"));
  rig.model.tool("report", {{"status", "fixed"}, {"cause", "c"}, {"fix", "f"}});
  auto r = rig.engine->debug("crackle", {});
  REQUIRE(r);
  CHECK(rig.model.calls[0].req.effort == "low");   // orchestrator: [effort]
  CHECK(rig.model.calls[0].req.max_tokens == cfg.max_output_tokens_routing);  // schema agents: short answers
  CHECK(rig.model.calls[2].req.max_tokens == cfg.max_output_tokens);          // the worker keeps the full limit
  CHECK(rig.model.calls[1].req.effort == "high");  // navigator on medium: the tier's effort
}

TEST_CASE("engine: tuning a constant of a shared block leaves its card valid (no hist line in a shared card)") {
  Rig rig;
  test::write(rig.p / "blocks/shared/types/api/consts.hpp", "#pragma once\nnamespace wk {\ninline constexpr int kRate = 48000;\n}\n");
  auto s = core::load_schema(rig.p.path());
  REQUIRE(s);
  index::StateStore st(rig.p.path());
  REQUIRE(st.freeze_api("shared.types", index::block_api_hash(index::collect_code_files(*s), "shared.types")));
  rig.model.tool("dispatch", dispatch("shared.types", "S", "a tunable number"));
  rig.model.tool("edit_file", {{"path", "api/consts.hpp"}, {"old", "48000"}, {"new", "44100"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "kRate 48000 -> 44100"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("use 44100 Hz");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(rig.ticket_text("T-0001"), "status  closed"));
  CHECK_FALSE(core::contains(test::read(rig.p / "blocks/shared/types/block.ksl"), "hist"));
  auto after = core::load_schema(rig.p.path());
  REQUIRE(after);
  CHECK_FALSE(core::has_errors(core::check_schema(*after)));
}

TEST_CASE("engine: answers cut at the output limit get a decide-now nudge with their last thoughts, each time") {
  Rig rig;
  auto cut = [](const std::string& thought) {
    return [thought](const llm::Request&) {
      llm::Response r;
      r.stop_reason = "max_tokens";
      r.usage = {10, 4096, 0, 0, 0};
      llm::ContentBlock b;
      b.type = llm::ContentBlock::Type::Other;
      b.text = thought;
      b.raw_json = R"({"type":"reasoning"})";
      r.content.push_back(b);
      return r;
    };
  };
  rig.model.then(cut("first: the pickups need a Slow kind"));
  rig.model.then(cut("second: so the change is a note line in rules.pickups"));
  rig.model.tool("close", {{"verdict", "not_a_bug"}});
  auto r = rig.engine->task("nothing");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 3);
  auto last = test::ScriptedModel::request_text(rig.model.calls[2].req);
  CHECK(core::contains(last, "cut at the output limit"));
  CHECK(core::contains(last, "second: so the change is a note line in rules.pickups"));
  CHECK(core::contains(rig.ticket_text("T-0001"), "status  closed"));
}

TEST_CASE("engine: an api/ unfrozen long ago and in line with its card is not presented as work to do") {
  Rig rig;
  index::StateStore st(rig.p.path());
  REQUIRE(st.freeze_api("stream.pump", "x"));  // ring unfrozen, its card unchanged (no D04)
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "a fix inside the ring"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "nothing in api/"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("tidy the ring");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto worker = test::ScriptedModel::request_text(rig.model.calls[1].req);
  CHECK(core::contains(worker, "unfrozen but already in line with its card"));
  CHECK_FALSE(core::contains(worker, "behind its card"));
  // New constants are never a card's business: allowed as in a frozen api/ (flappy T-0014, where
  // "only what the card names" made the worker of config refuse three new constants).
  CHECK(core::contains(worker, "add new constexpr constants and change values of constants when the ticket needs them"));
}

TEST_CASE("engine: the orchestrator of a feature sees the approved changes of its ticket as the agreed scope") {
  Rig rig;
  rig.model.tool("propose_change", {{"reason", "the ring can be reset by the pump"}, {"diff", "~ @block stream.ring\n+ fn reset() -> void"}});
  auto r = rig.engine->task("reset the stream on stop");
  REQUIRE(r);
  REQUIRE(rig.engine->approve("C-0001"));
  CHECK(core::contains(rig.ticket_text("T-0001"), "note    C-0001 approved"));
  rig.model.tool("close", {{"verdict", "done"}});
  auto again = rig.engine->run_ticket("T-0001");
  REQUIRE_MESSAGE(again, (again ? "" : again.error().str()));
  auto orch = test::ScriptedModel::request_text(rig.model.calls.back().req);
  CHECK(core::contains(orch, "Approved changes of this ticket (the agreed scope"));
  CHECK(core::contains(orch, "- C-0001 (stream.ring): the ring can be reset by the pump"));
}

TEST_CASE("engine: a run stopped after a finished leaf keeps that leaf's result lines") {
  Rig rig;
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "ring part"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "ring gains reset"}, {"files", {"impl/ring.cpp"}}});
  rig.model.tool("dispatch", dispatch("stream.pump", "S", "pump part"));
  rig.model.then([&](const llm::Request&) {
    for (const auto& a : metrics::active_runs(rig.p.path())) {
      if (a.role == "worker") REQUIRE(metrics::request_stop(rig.p.path(), a.journal));
    }
    return test::tool_response("read_file", {{"path", "impl/pump.cpp"}});
  });
  auto r = rig.engine->task("reset the stream on stop");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "cancelled");
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "status  open"));
  CHECK(core::contains(t, "status  stopped\nfix     stream.ring: ring gains reset\nfiles   stream.ring: impl/ring.cpp"));
  auto stopped_at = t.find("status  stopped");
  REQUIRE(stopped_at != std::string::npos);
  CHECK(t.find("\nstate   ", stopped_at) != std::string::npos);
  // Continued: the stopped result carries the state fingerprint, so the orchestrator hears that
  // nothing changed since, not that the result comes from "an older run".
  rig.model.tool("close", {{"verdict", "done"}, {"note", "the pump part comes in another ticket"}});
  auto again = rig.engine->run_ticket("T-0001");
  REQUIRE_MESSAGE(again, (again ? "" : again.error().str()));
  auto orch = test::ScriptedModel::request_text(rig.model.calls.back().req);
  CHECK(core::contains(orch, "Nothing changed in the cards or api/ since the last result."));
  CHECK_FALSE(core::contains(orch, "without a state fingerprint"));
}

TEST_CASE("engine: a feature that gives a leaf new invariants has the tester add their contract tests") {
  auto cfg = test::test_config();
  cfg.auto_approve = true;
  Rig rig(true, cfg);
  rig.model.tool("propose_change", {{"reason", "the ring can be reset"},
                                    {"diff", "~ @block stream.ring\n+ fn reset() -> void\n+ inv reset() leaves the ring empty"}});
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "implement reset"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "reset() implemented"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "contract test: reset leaves the ring empty"}, {"files", "tests/contract/ring_reset.cpp"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("the ring can be reset");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 5);
  auto tester = test::ScriptedModel::request_text(rig.model.calls[3].req);
  CHECK(core::contains(tester, "gained these invariants in this ticket"));
  CHECK(core::contains(tester, "- reset() leaves the ring empty"));
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "tester       at=stream.ring"));
  CHECK(core::contains(t, "tests   stream.ring: contract tests for 1 new invariant(s) by the tester"));
  CHECK(core::contains(t, "status  closed"));
}

TEST_CASE("engine: Auto (auto_approve) approves a proposed change and the orchestrator goes on in the same run") {
  auto cfg = test::test_config();
  cfg.auto_approve = true;
  Rig rig(true, cfg);
  rig.model.tool("propose_change", {{"reason", "the ring can be reset"}, {"diff", "~ @block stream.ring\n+ fn reset() -> void"}});
  rig.model.tool("dispatch", dispatch("stream.ring", "S", "implement reset"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "reset() implemented"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("the ring can be reset");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  agents::ChangeStore changes(rig.p.path());
  auto c = changes.load("C-0001");
  REQUIRE(c);
  CHECK(c->status.value == "approved");
  CHECK(core::contains(test::read(rig.p / "blocks/stream/ring/block.ksl"), "fn      reset() -> void"));
  auto t = rig.ticket_text("T-0001");
  CHECK(core::contains(t, "C-0001 approved automatically (auto_approve)"));
  CHECK(core::contains(t, "status  closed"));
  CHECK(core::contains(t, "fix     stream.ring: reset() implemented"));
}
