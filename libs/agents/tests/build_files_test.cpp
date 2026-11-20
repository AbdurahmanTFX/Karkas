// The build files of an imported project (docs/adr/0030): Karkas adds a block's new source file to the
// project's CMakeLists.txt, and a linker error names the block of its symbol.
#include <doctest/doctest.h>

#include "kk/agents/build_files.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

// Endless Sky's layout: source/CMakeLists.txt lists one file per line, sorted, relative to source/.
const std::string kOnePerLine =
    "target_sources(EndlessSkyLib PRIVATE\n"
    "\tPlanet.cpp\n"
    "\tPlanet.h\n"
    "\tPlanetLabel.cpp\n"
    "\tPlanetLabel.h\n"
    "\tPlayerInfoPanel.cpp\n"
    "\tPlayerInfoPanel.h\n"
    ")\n";

}  // namespace

TEST_CASE("build files: a new source and header go into a one-per-line list, sorted, once") {
  test::TempDir d;
  test::write(d / "CMakeLists.txt", "project(es)\nadd_subdirectory(source)\n");
  test::write(d / "source/CMakeLists.txt", kOnePerLine);
  test::write(d / "source/PilotStatisticsPanel.cpp", "// panel\n");
  test::write(d / "source/PilotStatisticsPanel.h", "#pragma once\n");
  auto added = agents::add_sources_to_build(
      d.path(), {"source/PlayerInfoPanel.cpp", "source/PilotStatisticsPanel.cpp", " source/PilotStatisticsPanel.h", "data/x.txt"});
  REQUIRE(added.size() == 1);
  CHECK(added[0] == "source/CMakeLists.txt: PilotStatisticsPanel.cpp, PilotStatisticsPanel.h");
  CHECK(test::read(d / "source/CMakeLists.txt") ==
        "target_sources(EndlessSkyLib PRIVATE\n"
        "\tPilotStatisticsPanel.cpp\n"
        "\tPilotStatisticsPanel.h\n"
        "\tPlanet.cpp\n"
        "\tPlanet.h\n"
        "\tPlanetLabel.cpp\n"
        "\tPlanetLabel.h\n"
        "\tPlayerInfoPanel.cpp\n"
        "\tPlayerInfoPanel.h\n"
        ")\n");
  // Named now: nothing more to add; the root list was never touched.
  CHECK(agents::add_sources_to_build(d.path(), {"source/PilotStatisticsPanel.cpp", "source/PilotStatisticsPanel.h"}).empty());
  CHECK(test::read(d / "CMakeLists.txt") == "project(es)\nadd_subdirectory(source)\n");
}

TEST_CASE("build files: a file sorting last goes after the last one of its folder") {
  test::TempDir d;
  test::write(d / "source/CMakeLists.txt", kOnePerLine);
  test::write(d / "source/Ship.cpp", "\n");
  CHECK(agents::add_sources_to_build(d.path(), {"source/Ship.cpp"}) ==
        std::vector<std::string>{"source/CMakeLists.txt: Ship.cpp"});
  CHECK(core::contains(test::read(d / "source/CMakeLists.txt"), "\tPlayerInfoPanel.h\n\tShip.cpp\n)\n"));
}

TEST_CASE("build files: files on one line of add_executable, paths from the root, headers only where headers are listed") {
  test::TempDir d;
  test::copy_tree(test::fixture("legacy"), d.path());
  test::write(d / "src/game/hud.cpp", "// hud\n");
  test::write(d / "src/game/hpp/hud.hpp", "#pragma once\n");
  auto added = agents::add_sources_to_build(d.path(), {"src/game/hud.cpp", "src/game/hpp/hud.hpp"});
  REQUIRE(added.size() == 1);
  CHECK(added[0] == "CMakeLists.txt: src/game/hud.cpp");
  auto text = test::read(d / "CMakeLists.txt");
  CHECK_MESSAGE(core::contains(text, "add_executable(game src/game/hud.cpp src/game/main.cpp src/game/world.cpp"), text);
  CHECK_FALSE(core::contains(text, "hud.hpp"));
}

TEST_CASE("build files: a missing file, a folder no list names and a glob are left alone") {
  test::TempDir d;
  test::write(d / "CMakeLists.txt", "file(GLOB SRC src/*.cpp)\nadd_executable(app ${SRC})\n");
  test::write(d / "src/a.cpp", "\n");
  CHECK(agents::add_sources_to_build(d.path(), {"src/a.cpp", "src/missing.cpp"}).empty());
  CHECK(test::read(d / "CMakeLists.txt") == "file(GLOB SRC src/*.cpp)\nadd_executable(app ${SRC})\n");
}

TEST_CASE("build files: the names of a linker error, most specific first") {
  using V = std::vector<std::string>;
  CHECK(agents::link_error_names("/usr/bin/ld: <artificial>:(.text+0xa39e): undefined reference to "
                                 "`PilotStatisticsPanel::PilotStatisticsPanel(PlayerInfo&)'") ==
        V{"PilotStatisticsPanel", "PilotStatisticsPanel"});
  CHECK(agents::link_error_names("a.o: multiple definition of `Twice(int)'; b.o: first defined here") == V{"Twice"});
  CHECK(agents::link_error_names("ld.lld: error: undefined symbol: vtable for Panel") == V{"Panel"});
  CHECK(agents::link_error_names("x.o: undefined reference to `ns::Map<int>::get() const'") == V{"ns", "Map"});
  CHECK(agents::link_error_names("source/X.cpp:3:1: error: expected ';'").empty());
}
