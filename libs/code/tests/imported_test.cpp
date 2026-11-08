// Blocks whose code lives in existing files ("files", docs/adr/0009): index roles,
// statuses, include resolution and drift checks without false alarms.
#include <doctest/doctest.h>

#include <algorithm>

#include "kk/code/analyzer.hpp"
#include "kk/code/drift.hpp"
#include "kk/code/gen.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/index/index.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

// The legacy fixture with hand-written cards over its existing files.
struct Imported : test::FixtureCopy {
  Imported() : FixtureCopy("legacy") {
    card("game", "purpose game objects\nkind group\n");
    card("game.main", "purpose entry point\nkind entry\nthread main\nfiles src/game/main.cpp\n"
                      "call game.world:update\ncall game.world:draw\ncall game.player:update\n"
                      "call game.player:position\ncall game.player:bag\ncall util.camera:follow\n");
    card("game.world", "purpose the island\nkind leaf\nthread main\nfiles src/game/world.cpp src/game/hpp/world.hpp\n"
                       "fn update(float) -> void\nfn is_walkable(Vec2) -> bool\nfn draw() -> void\ncall util.math:clampf\n");
    card("game.player", "purpose the player\nkind leaf\nthread main\nfiles src/game/player.cpp src/game/hpp/player.hpp\n"
                        "fn update(float, World) -> void\nfn position() -> Vec2\nfn bag() -> Inventory\n"
                        "call game.world:is_walkable\nuse game.inventory\nuse util.math\n");
    card("game.inventory", "purpose items\nkind shared\nfiles src/game/hpp/inventory.hpp\n");
    card("util", "purpose helpers\nkind group\n");
    card("util.camera", "purpose camera\nkind leaf\nthread main\nfiles src/util/camera.cpp src/util/hpp/camera.hpp\n"
                        "fn follow(Vec2, float) -> void\nfn screen_to_world(Vec2) -> Vec2\nfn zoom() -> float\n"
                        "call util.math:clampf\n");
    card("util.math", "purpose math\nkind leaf\nthread main\nfiles src/util/math.c src/util/math.h\n"
                      "fn clampf(float, float, float) -> float\n");
  }
  void card(const std::string& id, const std::string& body) {
    test::write(path() / "blocks" / core::id_to_rel_path(id) / "block.ksl", "@block " + id + "\n" + body);
  }
  core::Schema schema() {
    auto s = core::load_schema(path());
    REQUIRE_MESSAGE(s, (s ? "" : s.error().str()));
    return *s;
  }
};

}  // namespace

TEST_CASE("imported blocks: schema, index and drift are clean") {
  Imported p;
  auto s = p.schema();
  auto issues = core::check_schema(s);
  for (const auto& i : issues) MESSAGE(i.str());
  CHECK(issues.empty());  // no E09/E03; no W06 for imported leaves
  auto drift = code::check_drift(s);
  for (const auto& i : drift) MESSAGE(i.str());
  CHECK(drift.empty());  // no D02 (links match includes), D04 (Update ↔ update), D06, D08

  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  auto an = std::make_unique<code::CppAnalyzer>();
  REQUIRE((*idx)->update({false, an.get()}));
  auto snap = (*idx)->snapshot();
  for (const char* id : {"game.main", "game.world", "game.player", "game.inventory", "util.camera", "util.math"}) {
    INFO(id);
    REQUIRE(snap.block(id) != nullptr);
    CHECK(snap.block(id)->status == "implemented");
  }
  // Files keep their places; roles come from the extension.
  auto q = (*idx)->db().q("SELECT role FROM files WHERE path='src/game/hpp/world.hpp' AND block='game.world'");
  REQUIRE(q.step());
  CHECK(q.text(0) == "api");
  // Includes between imported blocks were resolved (relative paths).
  auto r = (*idx)->db().q("SELECT COUNT(*) FROM code_refs WHERE from_block='game.player' AND to_block='game.world' AND kind='include'");
  REQUIRE(r.step());
  CHECK(r.i64(0) == 1);
}

TEST_CASE("imported blocks: a missing link is still drift, generation leaves the code alone") {
  Imported p;
  p.card("game.player", "purpose the player\nkind leaf\nthread main\nfiles src/game/player.cpp src/game/hpp/player.hpp\n"
                        "fn update(float, World) -> void\nfn position() -> Vec2\nfn bag() -> Inventory\n"
                        "use game.inventory\nuse util.math\n");
  auto d = code::check_drift(p.schema());
  REQUIRE(!d.empty());
  CHECK(d[0].code == "D02");
  CHECK(d[0].block == "game.player");
  CHECK(core::contains(d[0].message, "includes game.world api without link"));
  // fn missing from the header is reported as usual.
  p.card("util.math", "purpose math\nkind leaf\nthread main\nfiles src/util/math.c src/util/math.h\n"
                      "fn clampf(float, float, float) -> float\nfn lerp(float, float, float) -> float\n");
  auto d4 = code::check_drift(p.schema());
  CHECK(std::any_of(d4.begin(), d4.end(), [](const core::Issue& i) { return i.code == "D04" && core::contains(i.message, "lerp"); }));
  // skel/mocks/probe never write into imported blocks.
  auto g = code::write_skel(p.schema(), "all");
  REQUIRE(g);
  CHECK(g->written.empty());
  auto m = code::write_mocks(p.schema(), "all");
  REQUIRE(m);
  CHECK(m->written.empty());
}

TEST_CASE("imported blocks: locate finds the block and the functions of a symptom (code symbols)") {
  Imported p;
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  auto an = std::make_unique<code::CppAnalyzer>();
  REQUIRE((*idx)->update({false, an.get()}));
  auto text = (*idx)->locate("I cannot walk onto some tiles of the island");
  INFO(text);
  auto first = text.find("1. ");
  REQUIRE(first != std::string::npos);
  CHECK(text.substr(first, 14) == "1. game.world ");
  CHECK(core::contains(text, "symbols "));
  CHECK(core::contains(text, "src/game/"));
}
