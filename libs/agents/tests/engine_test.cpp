#include <doctest/doctest.h>

#include "kk/agents/changes.hpp"
#include "kk/agents/engine.hpp"
#include "kk/code/import.hpp"
#include "kk/core/text.hpp"
#include "kk/core/tickets.hpp"
#include "kk/index/state.hpp"
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

TEST_CASE("engine: approving an import writes the cards, and agents leave imported code alone") {
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
  auto w = (*e)->run_worker("game.player", "", "make the player faster");
  CHECK(w.status == "failed");
  CHECK(core::contains(w.cause, "imported"));
  CHECK(model.calls.empty());  // no model call was spent on it
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
