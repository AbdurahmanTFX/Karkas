// Fewer and cheaper runs per ticket (docs/adr/0026): a feature planned in one decision, a worker
// that goes on in its run after Auto approves its card diff, constants and declarations written by
// the program, prompts that do not name the block (one cached prefix per role).
#include <doctest/doctest.h>
#include <fmt/format.h>

#include "kk/agents/api_edit.hpp"
#include "kk/agents/changes.hpp"
#include "kk/agents/engine.hpp"
#include "kk/core/text.hpp"
#include "kk/index/hash.hpp"
#include "kk/index/index.hpp"
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
  explicit Rig(core::Config cfg = test::test_config()) {
    agents::EngineOptions o;
    o.providers = model.factory();
    o.tests = &tests;
    auto e = agents::Engine::create(p.path(), cfg, o);
    REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
    engine = std::move(*e);
  }
  std::string ticket(const std::string& id = "T-0001") { return test::read(p / ".karkas/tickets" / (id + ".ksl")); }
};

json step(const std::string& block, const std::string& difficulty, const std::string& why) {
  return {{"block", block}, {"difficulty", difficulty}, {"why", why}};
}

core::Config auto_config() {
  auto cfg = test::test_config();
  cfg.auto_approve = true;
  return cfg;
}

}  // namespace

TEST_CASE("economy: a feature planned in one decision runs its blocks without asking the orchestrator again") {
  Rig rig;
  json first = step("stream.ring", "S", "ring part");
  first["next"] = json::array({step("stream.pump", "S", "pump part")});
  rig.model.tool("dispatch", first);
  rig.model.tool("report", {{"status", "done"}, {"fix", "ring gains reset"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "pump calls reset on stop"}});
  auto r = rig.engine->task("reset the stream on stop");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(rig.model.calls.size() == 3);  // one decision, two workers; no orchestrator after each leaf
  auto t = rig.ticket();
  CHECK(core::contains(t, "pick=stream.ring difficulty=S why=\"ring part\" plan=1/2 then=stream.pump"));
  CHECK(core::contains(t, "pick=stream.pump difficulty=S why=\"pump part\" plan=2/2"));
  CHECK(core::contains(t, "status  done\nfix     stream.ring: ring gains reset"));
  CHECK(core::contains(t, "the planned blocks are done: stream.ring, stream.pump"));
  CHECK(core::contains(t, "status  closed"));
}

TEST_CASE("economy: a block of the plan that does not end done brings the orchestrator back, with the rest of the plan") {
  Rig rig;
  json first = step("stream.ring", "S", "ring part");
  first["next"] = json::array({step("stream.pump", "S", "pump part")});
  rig.model.tool("dispatch", first);
  for (int i = 0; i < 2; ++i) rig.model.tool("report", {{"status", "not_here"}, {"cause", "the ring has no stop"}});
  rig.model.tool("close", {{"verdict", "wont_fix"}, {"note", "no stop to reset on"}});
  auto r = rig.engine->task("reset the stream on stop");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto back = test::ScriptedModel::request_text(rig.model.calls.back().req);
  CHECK(core::contains(back, "Planned and not sent yet: stream.pump (S)"));
}

TEST_CASE("economy: a plan that leaves out a block of the approved change goes back to the orchestrator") {
  Rig rig(auto_config());
  json change = {{"reason", "ring and pump reset"},
                 {"diff", "~ @block stream.ring\n+ fn reset() -> void\n~ @block stream.pump\n+ inv stop resets the ring"},
                 {"next", json::array({step("stream.ring", "S", "ring part")})}};
  rig.model.tool("propose_change", change);
  rig.model.tool("report", {{"status", "done"}, {"fix", "reset implemented"}});
  rig.model.tool("dispatch", step("stream.pump", "S", "pump part, left out of the plan"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "pump resets the ring on stop"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("reset the stream on stop");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(rig.ticket(), "the plan is done, but the approved changes of this ticket also change stream.pump"));
  CHECK(core::contains(rig.ticket(), "fix     stream.pump: pump resets the ring on stop"));
}

TEST_CASE("economy: a plan that waits for a human's approval goes on after it without the orchestrator") {
  Rig rig;  // no Auto
  json change = {{"reason", "the ring can be reset"},
                 {"diff", "~ @block stream.ring\n+ fn reset() -> void"},
                 {"next", json::array({step("stream.ring", "S", "implement reset")})}};
  rig.model.tool("propose_change", change);
  auto r = rig.engine->task("the ring can be reset");
  REQUIRE(r);
  CHECK(std::filesystem::exists(rig.p / ".karkas/tickets/T-0001.plan.json"));
  CHECK(core::contains(rig.ticket(), "planned after the approval of C-0001: stream.ring (S)"));
  REQUIRE(rig.engine->approve("C-0001"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "reset implemented"}});
  auto again = rig.engine->run_ticket("T-0001");
  REQUIRE_MESSAGE(again, (again ? "" : again.error().str()));
  CHECK(rig.model.calls.size() == 2);  // the orchestrator is not asked again
  CHECK(core::contains(rig.ticket(), "C-0001 approved: the plan goes on with stream.ring (S)"));
  CHECK(core::contains(rig.ticket(), "status  closed"));
  CHECK_FALSE(std::filesystem::exists(rig.p / ".karkas/tickets/T-0001.plan.json"));
}

TEST_CASE("economy: with Auto a worker's card diff is approved in its run and the worker goes on") {
  Rig rig(auto_config());
  rig.model.tool("dispatch", step("stream.ring", "S", "reset of the ring"));
  rig.model.tool("report", {{"status", "needs_contract_change"}, {"cause", "the card has no reset"},
                            {"proposal", "~ @block stream.ring\n+ fn reset() -> void"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "reset() declared and implemented"}, {"api", "changed"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("the ring can be reset");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 4);  // orchestrator, worker (2 steps of one run), orchestrator
  auto worker_after = test::ScriptedModel::request_text(rig.model.calls[2].req);
  CHECK(core::contains(worker_after, "C-0001 approved automatically (Auto). The card of stream.ring now reads:"));
  CHECK(core::contains(worker_after, "fn      reset() -> void"));
  agents::ChangeStore changes(rig.p.path());
  CHECK(changes.load("C-0001")->status.value == "approved");
  auto t = rig.ticket();
  CHECK(core::contains(t, "C-0001 approved automatically (auto_approve); the worker of stream.ring goes on in the same run"));
  CHECK(core::contains(t, "outcome=done"));
  CHECK(core::contains(t, "status  closed"));
}

TEST_CASE("economy: an approved change writes its constants into the api/ and keeps a frozen contract frozen") {
  Rig rig;
  auto root = rig.p.path();
  auto schema = core::load_schema(root);
  REQUIRE(schema);
  auto files = index::collect_code_files(*schema);
  index::StateStore st(root);
  REQUIRE(st.freeze_api("stream.ring", index::block_api_hash(files, "stream.ring")));
  auto c = agents::propose_change(root, "orchestrator T-0009", "a default capacity",
                                  "~ @block stream.ring\n+ const kDefaultCapacity std::size_t 4096 // frames");
  REQUIRE_MESSAGE(c, (c ? "" : c.error().str()));
  auto a = rig.engine->approve(c->id);
  REQUIRE_MESSAGE(a, (a ? "" : a.error().str()));
  CHECK(core::contains(*a, "stream.ring: std::size_t kDefaultCapacity = 4096 written to api/ring.hpp"));
  auto header = test::read(root / "blocks/stream/ring/api/ring.hpp");
  CHECK(core::contains(header, "inline constexpr std::size_t kDefaultCapacity = 4096;  // frames\n}  // namespace wk"));
  // The card is unchanged and the contract stays frozen, with the hash of the new header.
  CHECK_FALSE(core::contains(test::read(root / "blocks/stream/ring/block.ksl"), "kDefaultCapacity"));
  auto after = core::load_schema(root);
  REQUIRE(after);
  auto state = st.load();
  REQUIRE(state);
  REQUIRE(state->frozen_api.contains("stream.ring"));
  CHECK(state->frozen_api.at("stream.ring") == index::block_api_hash(index::collect_code_files(*after), "stream.ring"));
  // A malformed constant is refused with the way to write it.
  auto bad = agents::check_proposal(root, "~ @block stream.ring\n+ const 4096");
  REQUIRE_FALSE(bad);
  CHECK(core::contains(bad.error().message, "'+ const <name> <C++ type> <value> // note'"));
}

TEST_CASE("economy: an approved change declares a new fn whose types the header can take, in the header's style") {
  Rig rig;
  auto root = rig.p.path();
  auto c = agents::propose_change(root, "orchestrator T-0009", "reset and a name",
                                  "~ @block stream.ring\n+ fn reset() -> void\n+ fn label() -> string\n+ fn fancy(Gizmo) -> int");
  REQUIRE_MESSAGE(c, (c ? "" : c.error().str()));
  auto a = rig.engine->approve(c->id);
  REQUIRE_MESSAGE(a, (a ? "" : a.error().str()));
  auto header = test::read(root / "blocks/stream/ring/api/ring.hpp");
  // One interface of pure virtual methods: the new methods join it, std::string brings <string>.
  const bool joined =
      core::contains(header, "[[nodiscard]] virtual std::size_t fill() const = 0;\n  virtual std::string label() = 0;\n  virtual void reset() = 0;") ||
      core::contains(header, "[[nodiscard]] virtual std::size_t fill() const = 0;\n  virtual void reset() = 0;\n  virtual std::string label() = 0;");
  CHECK_MESSAGE(joined, header);
  CHECK(core::contains(header, "#include <string>"));
  CHECK_FALSE(core::contains(header, "fancy"));  // Gizmo is no type this header knows: left to the worker
  CHECK(core::contains(*a, "stream.ring: declared `virtual void reset() = 0;` in api/ring.hpp"));
}

TEST_CASE("economy: KSL types map to C++") {
  CHECK(agents::cxx_type("vector<Line>") == "std::vector<Line>");
  CHECK(agents::cxx_type("optional<Kind>") == "std::optional<Kind>");
  CHECK(agents::cxx_type("string") == "std::string");
  CHECK(agents::cxx_type("std::string") == "std::string");
  CHECK(agents::cxx_type("span<const Frame>") == "std::span<const Frame>");
}

TEST_CASE("economy: role prompts do not name the block, so one cached prefix serves every block of a role") {
  Rig rig;
  rig.model.tool("dispatch", step("stream.ring", "S", "ring"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "ok"}});
  rig.model.tool("dispatch", step("stream.pump", "S", "pump"));
  rig.model.tool("report", {{"status", "done"}, {"fix", "ok"}});
  rig.model.tool("close", {{"verdict", "done"}});
  REQUIRE(rig.engine->task("touch both"));
  REQUIRE(rig.model.calls.size() == 5);
  const auto& ring = rig.model.calls[1].req;
  const auto& pump = rig.model.calls[3].req;
  REQUIRE_FALSE(ring.system.empty());
  CHECK(ring.system.front().text == pump.system.front().text);
  CHECK(core::contains(ring.system.front().text, "WORKER of one leaf block: the block of the @view in your context"));
  // The orchestrator's prompt carries a short KSL legend; the whole reference is a tool.
  const auto& orch = rig.model.calls[0].req;
  CHECK_FALSE(core::contains(orch.system.front().text, "Records: @project <name>"));
  bool has_ref = false;
  for (const auto& t : orch.tools) has_ref = has_ref || t.name == "ksl_reference";
  CHECK(has_ref);
}

TEST_CASE("economy: an agent sees the declarations of a big neighbor header that its block uses, and tests as a list") {
  Rig rig;
  auto types = rig.p / "blocks/shared/types/api/types.hpp";
  auto text = test::read(types);
  // A big header: many constants, one of them used by the ring's impl.
  std::string more = "namespace wk {\n// Used by the ring.\ninline constexpr int kRingSlack = 4;\n";
  for (int i = 0; i < 120; ++i) more += fmt::format("inline constexpr int kUnused{} = {};  // filler\n", i, i);
  more += "}  // namespace wk\n";
  test::write(types, text + more);
  auto impl = rig.p / "blocks/stream/ring/impl/ring.cpp";
  test::write(impl, test::read(impl) + "\nstatic_assert(wk::kRingSlack > 0);\n");
  auto ctx = rig.engine->context_for(agents::Role::Worker, "stream.ring");
  CHECK(core::contains(ctx, "shared.types:api/types.hpp ("));
  CHECK(core::contains(ctx, "the declarations this block uses, read_file for the rest"));
  CHECK(core::contains(ctx, "// Used by the ring.\ninline constexpr int kRingSlack = 4;"));
  CHECK_FALSE(core::contains(ctx, "kUnused7 = 7"));
  // Unit tests: names, not their text.
  CHECK(core::contains(ctx, "=== tests/unit/ring_test.cpp ("));
  CHECK(core::contains(ctx, "\"capacity rounds up to a power of two\""));
  CHECK_FALSE(core::contains(ctx, "TEST_CASE(\"capacity rounds up"));
}

TEST_CASE("economy: review_changes asks the leaves of a change before approval, and a gap sends it back once") {
  auto cfg = test::test_config();
  cfg.review_changes = true;
  Rig rig(cfg);
  rig.model.tool("propose_change", {{"reason", "the ring can be reset"}, {"diff", "~ @block stream.ring\n+ inv reset() empties the ring"}});
  rig.model.tool("answer", {{"text", "no: the card lacks fn reset() -> void"}});
  rig.model.tool("escalate", {{"note", "amend later"}});
  auto r = rig.engine->task("the ring can be reset");
  REQUIRE(r);
  REQUIRE(rig.model.calls.size() == 3);
  auto ask = test::ScriptedModel::request_text(rig.model.calls[1].req);
  CHECK(core::contains(ask, "Is this card enough for your block's part of the ticket"));
  CHECK(core::contains(ask, "+ inv reset() empties the ring"));
  auto back = test::ScriptedModel::request_text(rig.model.calls[2].req);
  CHECK(core::contains(back, "The blocks of your change found gaps; it was set aside"));
  CHECK(core::contains(back, "stream.ring: no: the card lacks fn reset() -> void"));
  agents::ChangeStore changes(rig.p.path());
  CHECK(changes.load("C-0001")->status.value == "rejected");
}

TEST_CASE("economy: a tester with a task of a ticket escalates its tier when it fails, like a worker") {
  Rig rig;
  rig.model.tool("dispatch", step("stream.ring", "S", "contract test flakes"));
  rig.model.tool("report", {{"status", "needs_tester"}, {"cause", "the contract test samples fill too early"}});
  rig.model.tool("report", {{"status", "failed"}, {"cause", "cannot make it pass"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "the test samples fill after the last push"}});
  auto r = rig.engine->debug("ring contract test flakes", {});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 4);
  CHECK(rig.model.calls[2].tier == "cheap");
  CHECK(rig.model.calls[3].tier == "medium");
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[3].req), "previous attempts (do not repeat them)"));
  auto t = rig.ticket();
  CHECK(core::contains(t, "tester       at=stream.ring model=cheap attempt=1 outcome=failed"));
  CHECK(core::contains(t, "status  fixed"));
}
