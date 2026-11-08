#include <doctest/doctest.h>

#include "kk/agents/changes.hpp"
#include "kk/code/import.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/test/support.hpp"

using namespace kk;

TEST_CASE("changes: the spec example adds dsp.reverb and a call from dsp.chain (10.7)") {
  test::FixtureCopy p("wavkit");
  auto diff = agents::parse_diff(R"(+ @block dsp.reverb
+ purpose apply Schroeder reverb to frames
+ kind    leaf
+ thread  main
+ fn      process(Frame[]) -> void
+ use     shared.types
+ inv     zero mix leaves samples unchanged
~ @block dsp.chain
+ call    dsp.reverb:process)");
  REQUIRE(diff);
  agents::ChangeStore store(p.path());
  auto c = store.create("orchestrator T-0051", "reverb requested; no existing block provides it", *diff);
  REQUIRE(c);
  CHECK(c->id == "C-0001");
  CHECK(store.load("C-0001")->status.value == "pending");
  auto preview = agents::preview_diff(p.path(), *diff);
  CHECK(core::contains(preview, "+call    dsp.reverb:process"));
  CHECK(core::contains(preview, "--- /dev/null"));
  auto r = agents::apply_diff(p.path(), *diff);
  REQUIRE(r);
  CHECK(r->issues.empty());
  CHECK(r->files.size() == 2);
  auto s = core::load_schema(p.path());
  REQUIRE(s);
  REQUIRE(s->find("dsp.reverb") != nullptr);
  const auto* chain = s->find("dsp.chain");
  REQUIRE(chain->calls.size() == 4);
  CHECK(chain->calls[3].target.str() == "dsp.reverb:process");
  // The card stays canonical: the call sits with the other calls, before use.
  CHECK(core::contains(test::read(p / "blocks/dsp/chain/block.ksl"), "call    dsp.reverb:process\nuse     shared.types"));
  REQUIRE(store.set_status("C-0001", "approved"));
}

TEST_CASE("changes: check_diff runs the checks and writes nothing") {
  test::FixtureCopy p("wavkit");
  auto good = agents::parse_diff("+ @block dsp.reverb\n+ purpose apply reverb to frames\n+ kind leaf\n+ thread main\n"
                                 "+ fn process(Frame[]) -> void\n+ inv zero mix leaves samples unchanged\n");
  REQUIRE(good);
  auto r = agents::check_diff(p.path(), *good);
  REQUIRE(r);
  CHECK(r->issues.empty());
  CHECK(r->files == std::vector<std::string>{"blocks/dsp/reverb/block.ksl"});
  CHECK_FALSE(std::filesystem::exists(p / "blocks/dsp/reverb/block.ksl"));
  // A call to a block that does not exist is reported, nothing is written.
  auto bad = agents::parse_diff("~ @block dsp.chain\n+ call    nowhere.block:run\n");
  REQUIRE(bad);
  auto before = test::read(p / "blocks/dsp/chain/block.ksl");
  auto rb = agents::check_diff(p.path(), *bad);
  REQUIRE(rb);
  CHECK_FALSE(rb->issues.empty());
  CHECK(test::read(p / "blocks/dsp/chain/block.ksl") == before);
}

TEST_CASE("changes: removals, errors and schema checks before writing") {
  test::FixtureCopy p("wavkit");
  auto rm = agents::parse_diff("~ @block dsp.chain\n- call dsp.echo:process");
  REQUIRE(rm);
  REQUIRE(agents::apply_diff(p.path(), *rm));
  CHECK(core::load_schema(p.path())->find("dsp.chain")->calls.size() == 2);
  CHECK_FALSE(agents::parse_diff("call dsp.x:y"));
  CHECK_FALSE(agents::apply_diff(p.path(), *agents::parse_diff("~ @block nope\n+ note x")));
  CHECK_FALSE(agents::apply_diff(p.path(), *agents::parse_diff("~ @block dsp.chain\n- call dsp.none:process")));
  // A change that breaks the links (E03) writes nothing.
  auto before = test::read(p / "blocks/dsp/chain/block.ksl");
  auto bad = agents::apply_diff(p.path(), *agents::parse_diff("~ @block dsp.chain\n+ call dsp.missing:run"));
  REQUIRE(bad);
  CHECK_FALSE(bad->issues.empty());
  CHECK(bad->issues[0].code == "E03");
  CHECK(test::read(p / "blocks/dsp/chain/block.ksl") == before);
}

TEST_CASE("changes: karkas import goes through a change and applies cleanly") {
  test::FixtureCopy p("legacy");
  auto plan = code::plan_import(p.path());
  REQUIRE(plan);
  agents::ChangeStore store(p.path());
  auto c = store.create("import", "karkas import", plan->diff);
  REQUIRE(c);
  auto loaded = store.load(c->id);  // the change file parses back (files lines included)
  REQUIRE(loaded);
  CHECK(loaded->diff.size() == plan->diff.size());
  CHECK(core::contains(agents::preview_diff(p.path(), loaded->diff), "+files   src/game/hpp/player.hpp src/game/player.cpp"));
  auto r = agents::apply_diff(p.path(), loaded->diff);
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  for (const auto& i : r->issues) MESSAGE(i.str());
  CHECK(r->issues.empty());
  CHECK(r->files.size() == 8);
  auto s = core::load_schema(p.path());
  REQUIRE(s);
  CHECK(s->find("game.player")->external());
  CHECK(test::read(p / "src/game/player.cpp").find("Player::Update") != std::string::npos);  // code untouched
}
