#include <doctest/doctest.h>

#include "kk/agents/brief.hpp"
#include "kk/core/text.hpp"
#include "kk/runner/runner.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;

namespace {

struct BriefRig {
  test::FixtureCopy p{"wavkit"};
  core::Schema schema;
  std::unique_ptr<index::Index> idx;
  BriefRig() {
    auto s = core::load_schema(p.path());
    REQUIRE(s);
    schema = std::move(*s);
    auto i = index::Index::open(p.path());
    REQUIRE(i);
    idx = std::move(*i);
    REQUIRE(idx->update());
  }
};

void git(const std::filesystem::path& dir, std::vector<std::string> args) {
  std::vector<std::string> argv = {"git", "-C", dir.string(), "-c", "user.name=t", "-c", "user.email=t@t"};
  argv.insert(argv.end(), args.begin(), args.end());
  auto r = runner::run({argv, dir, std::chrono::seconds(30)});
  REQUIRE(r);
  REQUIRE_MESSAGE(r->ok(), r->output);
}

}  // namespace

TEST_CASE("brief: everything Karkas knows about the block, [fill] lines for the delegating model") {
  BriefRig rig;
  auto cfg = test::test_config();
  auto b = agents::make_brief(rig.p.path(), rig.schema, cfg, rig.idx->snapshot(), {}, "dsp.chain");
  REQUIRE_MESSAGE(b, (b ? "" : b.error().str()));
  INFO(*b);
  CHECK(b->starts_with("# Brief for karkas-cheap (m-cheap) — block dsp.chain"));
  CHECK(core::contains(*b, "Task: [fill:"));
  CHECK(core::contains(*b, "Spec: [fill:"));
  CHECK(core::contains(*b, "Done when: [fill:"));
  CHECK(core::contains(*b, "blocks/dsp/chain/impl/"));                        // files to change
  CHECK(core::contains(*b, "- an empty chain passes frames through unchanged"));  // guarantees
  CHECK(core::contains(*b, "dsp.gain [blocks/dsp/gain/api/]: process(Frame[]) -> void"));  // api it may use
  CHECK(core::contains(*b, "stream.ring"));                                   // data input supplier
  CHECK(core::contains(*b, "kk_run_tests dsp.chain"));
  CHECK(core::contains(*b, "Do not touch"));
  CHECK(core::contains(*b, "Project rules:"));
  auto t = agents::make_brief(rig.p.path(), rig.schema, cfg, rig.idx->snapshot(), {}, "dsp.chain", "add a bypass switch");
  CHECK(core::contains(*t, "Task: add a bypass switch"));
  CHECK_FALSE(agents::make_brief(rig.p.path(), rig.schema, cfg, rig.idx->snapshot(), {}, "dsp"));  // a group
  CHECK_FALSE(agents::make_brief(rig.p.path(), rig.schema, cfg, rig.idx->snapshot(), {}, "nowhere"));
}

TEST_CASE("review: guarantees, changes inside and outside the block, the diff, issues") {
  BriefRig rig;
  git(rig.p.path(), {"init", "-q"});
  git(rig.p.path(), {"add", "-A"});
  git(rig.p.path(), {"commit", "-q", "-m", "start"});
  auto impl = rig.p / "blocks/dsp/chain/impl/chain.cpp";
  test::write(impl, test::read(impl) + "// reviewed change\n");
  test::write(rig.p / "blocks/mix/impl/mix.cpp", test::read(rig.p / "blocks/mix/impl/mix.cpp") + "// stray\n");
  auto r = agents::make_review(rig.p.path(), rig.schema, rig.idx->snapshot(), "dsp.chain");
  REQUIRE(r);
  INFO(*r);
  CHECK(core::contains(*r, "[ ] an empty chain passes frames through unchanged"));
  CHECK(core::contains(*r, "blocks/dsp/chain/impl/chain.cpp"));
  CHECK(core::contains(*r, "Outside this block — justify or undo:\n  blocks/mix/impl/mix.cpp"));
  CHECK(core::contains(*r, "+// reviewed change"));
  CHECK_FALSE(core::contains(*r, "+// stray"));  // the diff is the block's own
  CHECK(core::contains(*r, "Issues of the block"));
  CHECK(core::contains(*r, "Accept only if"));
}
