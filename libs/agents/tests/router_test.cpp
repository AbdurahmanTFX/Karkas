#include <doctest/doctest.h>

#include "kk/agents/router.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;

namespace {

struct ModelRig {
  test::FixtureCopy p{"wavkit"};
  core::Schema schema;
  std::unique_ptr<index::Index> idx;
  ModelRig() {
    auto s = core::load_schema(p.path());
    REQUIRE(s);
    schema = std::move(*s);
    auto i = index::Index::open(p.path());
    REQUIRE(i);
    idx = std::move(*i);
    REQUIRE(idx->update({true, nullptr}));
  }
  agents::ModelChoice pick(const core::Config& cfg, std::string_view block, std::string role = "") {
    auto c = agents::model_for(cfg, schema, idx->snapshot(), {}, block, std::move(role));
    REQUIRE_MESSAGE(c, (c ? "" : c.error().str()));
    return *c;
  }
};

}  // namespace

TEST_CASE("model_for: the same order as Karkas's own agents (ADR 0011, 0013)") {
  ModelRig rig;
  auto cfg = test::test_config();
  // Nothing pinned: a leaf is a worker routed by size, a group or the project an architect.
  auto pump = rig.pick(cfg, "stream.pump");
  CHECK(pump.role == "worker");
  CHECK(pump.level == 2);
  CHECK(pump.tier == "cheap");
  CHECK(pump.by.starts_with("routing score="));
  CHECK(pump.model.model == "m-cheap");
  CHECK(pump.claude_model == "m-cheap");
  CHECK(pump.subagent == "karkas-cheap");
  auto root = rig.pick(cfg, "");
  CHECK(root.role == "architect");
  CHECK(root.tier == "strong");
  CHECK(root.by == "role architect");
  CHECK(rig.pick(cfg, "stream").role == "architect");
  CHECK(rig.pick(cfg, "stream.pump", "navigator").tier == "medium");
  // [roles] beats routing when it is not "auto".
  cfg.roles["worker"] = "medium";
  auto w = rig.pick(cfg, "stream.pump");
  CHECK(w.tier == "medium");
  CHECK(w.by == "role worker");
  // [levels], then [blocks] (the nearest ancestor) beat the role.
  cfg.levels = {{0, "medium"}, {2, "strong"}};
  cfg.block_tiers = {{"stream", "cheap"}};
  CHECK(rig.pick(cfg, "").by == "pinned by level 0");
  auto dsp = rig.pick(cfg, "dsp.echo");
  CHECK(dsp.tier == "strong");
  CHECK(dsp.by == "pinned by level 2");
  auto ring = rig.pick(cfg, "stream.ring", "tester");
  CHECK(ring.tier == "cheap");
  CHECK(ring.by == "pinned by block stream");
}

TEST_CASE("model_for: tiers Claude Code cannot run, unknown tiers, blocks and roles") {
  ModelRig rig;
  auto cfg = test::test_config();
  core::ModelTier local;
  local.provider = "openai_compat";
  local.model = "qwen3-coder";
  local.base_url = "http://localhost:11434/v1";
  cfg.models["local"] = local;
  cfg.block_tiers = {{"dsp", "local"}, {"io", "missing"}};
  auto d = rig.pick(cfg, "dsp.lowpass");
  CHECK(d.defined);
  CHECK(d.claude_model.empty());
  CHECK(d.subagent.empty());
  CHECK(core::contains(agents::model_choice_text(d), "not a Claude model"));
  auto m = rig.pick(cfg, "io.reader");
  CHECK_FALSE(m.defined);
  CHECK(core::contains(agents::model_choice_text(m), "has no [models.missing]"));
  auto ok = agents::model_choice_text(rig.pick(cfg, "stream.pump"));
  CHECK(core::contains(ok, "subagent karkas-cheap (model m-cheap)"));
  CHECK_FALSE(agents::model_for(cfg, rig.schema, rig.idx->snapshot(), {}, "nowhere"));
  CHECK_FALSE(agents::model_for(cfg, rig.schema, rig.idx->snapshot(), {}, "dsp", "poet"));
}
