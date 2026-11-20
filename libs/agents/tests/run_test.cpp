// "Build and run" (docs/adr/0037): which program runs, the build that makes it, the programs a project
// with its own build may name.
#include <doctest/doctest.h>

#include "kk/agents/run.hpp"
#include "kk/agents/testing.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

core::Schema load(const test::TempDir& d) {
  auto s = core::load_schema(d.path());
  REQUIRE(s);
  return std::move(*s);
}

}  // namespace

TEST_CASE("run: the program of a generated project is its entry block's executable, run_command wins") {
  test::TempDir d{"run_gen"};
  test::write(d / "project.ksl", "@project tone\nlang cpp\nbuild cmake-ninja\nstd c++20\ntest doctest\n");
  test::write(d / "blocks/app/block.ksl", "@block app\npurpose plays a tone\nkind entry\nthread main\n");
  core::Config cfg;
  auto t = agents::run_target(load(d), cfg);
  CHECK_FALSE(t.ok());
  CHECK(core::contains(t.error, "the entry block app has no code yet"));
  CHECK(core::contains(t.error, "run_command"));
  CHECK(t.generated.empty());

  test::write(d / "blocks/app/impl/main.cpp", "int main() { return 0; }\n");
  t = agents::run_target(load(d), cfg, {"--freq", "440 Hz"});
  REQUIRE(t.ok());
  CHECK_FALSE(t.command);
  CHECK(t.generated == "build/tone");
  CHECK(t.argv == std::vector<std::string>{(d.path() / "build/tone").string(), "--freq", "440 Hz"});
  CHECK(t.shown == "build/tone --freq '440 Hz'");

  cfg.build_dir = "out";
  CHECK(agents::run_target(load(d), cfg).shown == "out/tone");

  cfg.run_command = "out/tone --loud";
  t = agents::run_target(load(d), cfg);
  REQUIRE(t.ok());
  CHECK(t.command);
  CHECK(t.argv == std::vector<std::string>{"/bin/sh", "-c", "out/tone --loud", "sh"});
  CHECK(t.generated == "out/tone");  // what the command replaced
  t = agents::run_target(load(d), cfg, {"a b"});
  CHECK(t.argv == std::vector<std::string>{"/bin/sh", "-c", "out/tone --loud \"$@\"", "sh", "a b"});
  CHECK(t.shown == "out/tone --loud 'a b'");
}

TEST_CASE("run: a project with its own build names its program, the build folder offers the programs it holds") {
  test::TempDir d{"run_own"};
  test::write(d / "project.ksl", "@project own\nlang cpp\nbuild cmake-ninja\nstd c++20\ntest doctest\n");
  test::write(d / "blocks/app/block.ksl", "@block app\npurpose runs\nkind entry\nthread main\nfiles src/main.cpp\n");
  test::write(d / "src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"hello\"); }\n");
  test::write(d / "src/lib.cpp", "int answer() { return 42; }\n");
  test::write(d / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(own CXX)\nadd_library(answer SHARED src/lib.cpp)\n"
              "add_executable(game src/main.cpp)\nset_target_properties(game PROPERTIES RUNTIME_OUTPUT_DIRECTORY bin)\n");
  const auto cmake = test::read(d / "CMakeLists.txt");
  core::Config cfg;
  auto t = agents::run_target(load(d), cfg);
  CHECK_FALSE(t.ok());
  CHECK(core::contains(t.error, "its own build"));
  CHECK(agents::program_candidates(d.path(), cfg).empty());  // nothing built yet

  agents::TestService svc(d.path(), cfg);
  std::string streamed;
  auto b = svc.build_program([&](std::string_view s) { streamed += s; });
  REQUIRE_MESSAGE(b, (b ? "" : b.error().message));
  CHECK_MESSAGE(b->ok, b->log);
  CHECK(core::contains(streamed, "Linking"));  // configure and build came through as they ran
  CHECK(test::read(d / "CMakeLists.txt") == cmake);  // never written

  auto c = agents::program_candidates(d.path(), cfg);
  CHECK(c == std::vector<std::string>{"build/bin/game"});  // no library, no CMake compiler checks
  auto help = agents::run_target_help(t, c);
  CHECK(core::contains(help, "  build/bin/game\n"));
  CHECK(core::contains(help, "run_command = \"build/bin/game\""));

  cfg.run_command = "build/bin/game";
  t = agents::run_target(load(d), cfg);
  CHECK(t.ok());
  CHECK(t.generated.empty());
}

TEST_CASE("run: the build of a project with a build command streams its output and keeps the errors") {
  test::TempDir d{"run_cmd"};
  test::write(d / "project.ksl", "@project own\nlang cpp\nbuild cmake-ninja\nstd c++20\ntest doctest\n");
  core::Config cfg;
  cfg.build_command = "echo compiling; echo 'src/a.cpp:3:1: error: expected ;' 1>&2; exit 2";
  agents::TestService svc(d.path(), cfg);
  std::string streamed;
  int waited = 0;
  auto b = svc.build_program([&](std::string_view s) { streamed += s; }, [&] { ++waited; });
  REQUIRE(b);
  CHECK_FALSE(b->ok);
  CHECK(core::contains(streamed, "compiling"));
  CHECK(core::contains(b->summary, "src/a.cpp:3:1: error: expected ;"));
  CHECK(waited == 0);  // nobody else was building
  CHECK_FALSE(std::filesystem::exists(d / "CMakeLists.txt"));
}

TEST_CASE("run: words for a shell") {
  CHECK(agents::shell_quote("build/app") == "build/app");
  CHECK(agents::shell_quote("--gain=0.5") == "--gain=0.5");
  CHECK(agents::shell_quote("two words") == "'two words'");
  CHECK(agents::shell_quote("it's") == "'it'\\''s'");
  CHECK(agents::shell_quote("") == "''");
}
