// karkas import (docs/adr/0009): existing code → cards with "files", deterministic,
// and the imported schema passes the schema and drift checks right away.
#include <doctest/doctest.h>

#include <algorithm>

#include "kk/code/analyzer.hpp"
#include "kk/code/drift.hpp"
#include "kk/code/import.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/index/index.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

const core::Block* find(const code::ImportPlan& p, std::string_view id) {
  for (const auto& b : p.blocks) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

// Writes the planned cards the way an approved change would.
void write_cards(const std::filesystem::path& root, const code::ImportPlan& plan) {
  for (const auto& b : plan.blocks) {
    test::write(root / "blocks" / core::id_to_rel_path(b.id) / "block.ksl", core::write_block(b));
  }
}

std::string diff_text(const code::ImportPlan& plan) {
  std::string out;
  for (const auto& d : plan.diff) out += std::string(1, d.op) + " " + d.text + "\n";
  return out;
}

}  // namespace

TEST_CASE("import: folders become groups, file pairs become leaves") {
  test::FixtureCopy p("legacy");
  auto plan = code::plan_import(p.path());
  REQUIRE_MESSAGE(plan, (plan ? "" : plan.error().str()));
  CHECK(plan->groups == 2);
  CHECK(plan->leaves == 6);
  CHECK(plan->skipped == std::vector<std::string>{"third_party"});
  CHECK(std::find(plan->files.begin(), plan->files.end(), "third_party/tiny/tiny.cpp") == plan->files.end());

  const auto* player = find(*plan, "game.player");
  REQUIRE(player != nullptr);
  CHECK(player->kind == core::BlockKind::Leaf);
  CHECK(player->file_list() == std::vector<std::string>{"src/game/hpp/player.hpp", "src/game/player.cpp"});
  CHECK(player->purpose.value == "The player: walks on walkable tiles and carries an inventory");
  REQUIRE(player->ports.size() == 3);  // public only: Update, Position, Bag
  CHECK(player->ports[0].signature() == "update(float, World)");
  CHECK(player->ports[1].signature() == "position() -> Vec2");
  REQUIRE(player->calls.size() == 1);
  CHECK(player->calls[0].target.str() == "game.world:is_walkable");
  REQUIRE(player->uses.size() == 2);  // types only: inventory, math
  CHECK(player->uses[0].target == "game.inventory");

  // Private methods (Clamp) are not ports; the first comment sentence is the purpose.
  const auto* camera = find(*plan, "util.camera");
  REQUIRE(camera != nullptr);
  CHECK(camera->purpose.value == "Camera wrapper: smooth follow and world bounds");
  CHECK(camera->ports.size() == 3);
  CHECK(std::none_of(camera->ports.begin(), camera->ports.end(), [](const core::Port& x) { return x.name == "clamp"; }));

  const auto* main = find(*plan, "game.main");
  REQUIRE(main != nullptr);
  CHECK(main->kind == core::BlockKind::Entry);
  CHECK(main->ports.empty());
  CHECK(main->calls.size() == 6);

  // A header with types only and no calls is shared; C files pair with their header.
  CHECK(find(*plan, "game.inventory")->kind == core::BlockKind::Shared);
  CHECK(find(*plan, "util.math")->file_list() == std::vector<std::string>{"src/util/math.c", "src/util/math.h"});
  CHECK(find(*plan, "util")->purpose.value == "src/util: camera, math");

  CHECK_MESSAGE(test::matches_golden("import/legacy.txt", code::describe_plan(*plan) + "\n" + diff_text(*plan)),
                code::describe_plan(*plan));
}

TEST_CASE("import: the imported schema passes the checks and indexes as implemented") {
  test::FixtureCopy p("legacy");
  auto plan = code::plan_import(p.path());
  REQUIRE(plan);
  write_cards(p.path(), *plan);
  auto s = core::load_schema(p.path());
  REQUIRE(s);
  auto issues = core::check_schema(*s);
  for (const auto& i : issues) MESSAGE(i.str());
  CHECK(std::none_of(issues.begin(), issues.end(), [](const core::Issue& i) { return i.severity == 'E'; }));
  auto drift = code::check_drift(*s);
  for (const auto& i : drift) MESSAGE(i.str());
  CHECK(drift.empty());

  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  code::CppAnalyzer an;
  REQUIRE((*idx)->update({false, &an}));
  auto snap = (*idx)->snapshot();
  CHECK(snap.block("game.player")->status == "implemented");
  CHECK(snap.block("game")->status == "implemented");

  // Everything is claimed now: a second import finds nothing.
  auto again = code::plan_import(p.path());
  REQUIRE(again);
  CHECK(again->blocks.empty());
}

TEST_CASE("import: modules called builder or build_steps are cards like any other, build trees are not code") {
  // leveldb's db/builder.cc: its card was written to blocks/db/builder and never loaded (a folder starting
  // with "build" was taken for a build tree), so the import's own check reported E03 at once.
  test::TempDir d{"builder"};
  test::write(d / "project.ksl", "@project lv\nlang cpp\nbuild cmake-ninja\nstd c++20\ntest doctest\n");
  test::write(d / "db/builder.h", "#pragma once\nnamespace lv {\n// Writes a table.\nint BuildTable(int n);\n}\n");
  test::write(d / "db/builder.cc", "#include \"db/builder.h\"\nnamespace lv {\nint BuildTable(int n) { return n; }\n}\n");
  test::write(d / "db/build_steps.h", "#pragma once\nnamespace lv {\nint Steps();\n}\n");
  test::write(d / "db/build_steps.cc", "#include \"db/build_steps.h\"\nnamespace lv {\nint Steps() { return 2; }\n}\n");
  test::write(d / "db/db_impl.h", "#pragma once\nnamespace lv {\nint Open();\n}\n");
  test::write(d / "db/db_impl.cc",
              "#include \"db/db_impl.h\"\n#include \"db/builder.h\"\n#include \"db/build_steps.h\"\n"
              "namespace lv {\nint Open() { return BuildTable(1) + Steps(); }\n}\n");
  test::write(d / "main.cc", "#include \"db/db_impl.h\"\nint main() { return lv::Open(); }\n");
  test::write(d / "build-release/CMakeFiles/gen.cc", "int generated() { return 0; }\n");
  auto plan = code::plan_import(d.path());
  REQUIRE_MESSAGE(plan, (plan ? "" : plan.error().str()));
  REQUIRE(find(*plan, "db.builder") != nullptr);
  REQUIRE(find(*plan, "db.build_steps") != nullptr);
  CHECK(std::none_of(plan->files.begin(), plan->files.end(), [](const std::string& f) { return f.starts_with("build-release/"); }));
  write_cards(d.path(), *plan);
  auto s = core::load_schema(d.path());
  REQUIRE(s);
  CHECK(s->blocks.size() == plan->blocks.size());
  CHECK(s->find("db.builder") != nullptr);
  CHECK(s->find("db.build_steps") != nullptr);
  auto issues = core::check_schema(*s);
  for (const auto& i : issues) MESSAGE(i.str());
  CHECK(std::none_of(issues.begin(), issues.end(), [](const core::Issue& i) { return i.severity == 'E'; }));
  auto drift = code::check_drift(*s);
  for (const auto& i : drift) MESSAGE(i.str());
  CHECK(std::none_of(drift.begin(), drift.end(), [](const core::Issue& i) { return i.code == "D06"; }));
}

TEST_CASE("import: a library without main() gets an empty entry, --exclude skips folders") {
  test::TempDir dir("imp");
  test::write(dir / "project.ksl", "@project lib\nidea    a library\n");
  test::write(dir / "include/lib/parser.hpp", "#pragma once\n// Parses the input.\nint parse(const char* text);\n");
  test::write(dir / "src/parser.cpp", "#include \"lib/parser.hpp\"\nint parse(const char* t) { return t ? 1 : 0; }\n");
  test::write(dir / "tools/gen.cpp", "int helper() { return 2; }\n");
  code::ImportOptions opts;
  opts.exclude = {"tools"};
  auto plan = code::plan_import(dir.path(), opts);
  REQUIRE(plan);
  // include/lib/parser.hpp + src/parser.cpp: one module found by the common stem.
  const auto* parser = find(*plan, "parser");
  REQUIRE(parser != nullptr);
  CHECK(parser->file_list() == std::vector<std::string>{"include/lib/parser.hpp", "src/parser.cpp"});
  CHECK(parser->purpose.value == "Parses the input");
  REQUIRE(find(*plan, "app") != nullptr);
  CHECK(find(*plan, "app")->kind == core::BlockKind::Entry);
  CHECK(plan->skipped == std::vector<std::string>{"tools"});
  CHECK(std::any_of(plan->warnings.begin(), plan->warnings.end(),
                    [](const std::string& w) { return core::contains(w, "no main() found"); }));
}
