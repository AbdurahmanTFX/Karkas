// Requests the human limits to a block, and messages about a ticket (docs/adr/0034): the chat of
// karkas-gui in block mode runs `karkas task --block <id>`, in ticket mode `karkas ticket run <id>
// --note "..."`.
#include <doctest/doctest.h>

#include "kk/agents/engine.hpp"
#include "kk/core/text.hpp"
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
  Rig() {
    agents::EngineOptions o;
    o.providers = model.factory();
    o.tests = &tests;
    auto e = agents::Engine::create(p.path(), test::test_config(), o);
    REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
    engine = std::move(*e);
  }
  std::string ticket(const std::string& id = "T-0001") { return test::read(p / ".karkas/tickets" / (id + ".ksl")); }
  std::string request(std::size_t i) { return test::ScriptedModel::request_text(model.calls.at(i).req); }
};

}  // namespace

TEST_CASE("scope: a request limited to a leaf starts at its worker, without an orchestrator decision first") {
  Rig rig;
  rig.model.tool("report", {{"status", "done"}, {"fix", "reset() implemented"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("the ring can be reset", "stream.ring");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 2);  // the worker, then the orchestrator closes the feature
  CHECK(core::contains(rig.request(0), "the ring can be reset"));
  CHECK_FALSE(core::contains(rig.request(0), "Decide: dispatch"));  // the first run was a worker's
  CHECK(core::contains(rig.request(1), "The human limited this ticket to stream.ring"));
  auto t = rig.ticket();
  CHECK(core::contains(t, "scope   stream.ring"));
  CHECK(core::contains(t, "L0 human        at=root pick=stream.ring why=\"the human limited the request to this block\""));
  CHECK(core::contains(t, "status  closed"));
}

TEST_CASE("scope: a request limited to a group starts at its navigator and the orchestrator dispatches only inside it") {
  Rig rig;
  rig.model.tool("dispatch", {{"block", "stream.ring"}, {"difficulty", "S"}, {"why", "the ring keeps the frames"}});  // navigator at stream
  rig.model.tool("report", {{"status", "done"}, {"fix", "the ring can be reset"}});
  rig.model.tool("dispatch", {{"block", "dsp"}, {"difficulty", "S"}, {"why", "the effect chain too"}});  // orchestrator: refused, outside
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("reset the stream on stop", "stream");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 4);
  CHECK(core::contains(rig.request(0), "Decide: dispatch to a child or report not_here"));  // the navigator of stream first
  // The refused dispatch came back to the orchestrator as a tool error, in the same run.
  CHECK(core::contains(rig.request(3), "the human limited this ticket to stream"));
  auto t = rig.ticket();
  CHECK(core::contains(t, "L1 navigator    at=stream"));
  CHECK_FALSE(core::contains(t, "pick=dsp"));
  CHECK(core::contains(t, "status  closed"));
}

TEST_CASE("scope: an unknown block is refused before a ticket exists") {
  Rig rig;
  auto r = rig.engine->task("anything", "stream.nowhere");
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "unknown block stream.nowhere"));
  CHECK_FALSE(std::filesystem::exists(rig.p / ".karkas/tickets/T-0001.ksl"));
  CHECK(rig.model.calls.empty());
}

TEST_CASE("scope: a message about a ticket becomes a fact of the human and runs it again, also when closed") {
  Rig rig;
  rig.model.tool("close", {{"verdict", "done"}, {"note", "reset exists already"}});
  REQUIRE(rig.engine->task("the ring can be reset"));
  CHECK(core::contains(rig.ticket(), "status  closed"));
  rig.model.tool("close", {{"verdict", "done"}, {"note", "seek resets too"}});
  auto r = rig.engine->run_ticket("T-0001", false, "also reset the ring on seek");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 2);
  CHECK(core::contains(rig.request(1), "human: also reset the ring on seek"));
  CHECK(core::contains(rig.ticket(), "fact    human: also reset the ring on seek"));
  // Without a message a closed ticket still needs --force.
  auto again = rig.engine->run_ticket("T-0001");
  REQUIRE(again);
  CHECK(core::contains(*again, "is closed"));
  CHECK(rig.model.calls.size() == 2);
}
