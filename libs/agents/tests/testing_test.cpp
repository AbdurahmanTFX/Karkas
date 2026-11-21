#include <doctest/doctest.h>

#include <fmt/format.h>

#include "kk/agents/testing.hpp"
#include "kk/core/text.hpp"
#include "kk/templates.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

// A project with its own CMakeLists.txt, like imported code (ADR 0016).
void own_project(const test::TempDir& d) {
  test::write(d / "project.ksl", "@project own\nlang cpp\nbuild cmake-ninja\nstd c++20\ntest doctest\n");
  test::write(d / "blocks/app/block.ksl", "@block app\npurpose runs\nkind entry\nthread main\nfiles src/main.cpp\n");
  test::write(d / "src/main.cpp", "int main() { return 0; }\n");
  test::write(d / "src/pass.cpp", "#include <cstdio>\nint main() { std::puts(\"\\x1b[1;32m*** No errors detected\\x1b[0m\"); }\n");
  test::write(d / "src/fail.cpp",
              "#include <cstdio>\nint main() { std::puts(\"noise\"); std::puts(\"check x == 2 has failed [1 != 2]\"); return 201; }\n");
  test::write(d / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(own CXX)\nadd_executable(app src/main.cpp)\n"
              "if(WITH_TESTS)\n  add_executable(pass src/pass.cpp)\n  add_executable(fail src/fail.cpp)\nendif()\n");
}

}  // namespace

TEST_CASE("testing: a project with its own CMakeLists.txt is built as it is and runs its own tests") {
  test::TempDir d{"own"};
  own_project(d);
  const auto cmake = test::read(d / "CMakeLists.txt");
  core::Config cfg;
  cfg.cmake_args = "-DWITH_TESTS=ON";
  // No [test] command and nothing registered with ctest: say how to configure it.
  {
    agents::TestService svc(d.path(), cfg);
    auto r = svc.build_and_test({});
    REQUIRE_MESSAGE(r, (r ? "" : r.error().message));
    CHECK(r->build_ok);
    CHECK(core::contains(r->text, "set [test] command"));
  }
  CHECK(test::read(d / "CMakeLists.txt") == cmake);  // never written
  cfg.test_command = "build/pass";
  {
    agents::TestService svc(d.path(), cfg);
    auto r = svc.build_and_test({"app"});
    REQUIRE(r);
    CHECK(r->ok);
    CHECK(core::contains(r->text, "tests (build/pass): ok"));
    CHECK(core::contains(r->text, "*** No errors detected"));
    CHECK_FALSE(core::contains(r->text, "\x1b["));  // colours removed
    CHECK(core::contains(r->text, "run as a whole"));
  }
  cfg.test_command = "build/fail";
  {
    agents::TestService svc(d.path(), cfg);
    auto r = svc.build_and_test({});
    REQUIRE(r);
    CHECK_FALSE(r->ok);
    CHECK(core::contains(r->text, "FAILED (exit 201)"));
    CHECK(core::contains(r->text, "has failed [1 != 2]"));
  }
}

TEST_CASE("testing: a project's own build command builds it, Karkas configures nothing and collects warnings (ADR 0027)") {
  test::TempDir d{"owncmd"};
  own_project(d);
  core::Config cfg;
  cfg.build_command = "echo 'src/main.cpp:1:5: warning: unused variable [-Wunused-variable]' && echo '[1/1] Linking app'";
  cfg.test_command = "true";
  {
    agents::TestService svc(d.path(), cfg);
    auto r = svc.build_and_test({"app"});
    REQUIRE_MESSAGE(r, (r ? "" : r.error().message));
    CHECK(r->build_ok);
    CHECK(r->ok);
    CHECK(r->build_warnings == "src/main.cpp:1:5: warning: unused variable [-Wunused-variable]");
  }
  CHECK_FALSE(std::filesystem::exists(d / "build/CMakeCache.txt"));  // no configure by Karkas
  cfg.build_command = "echo 'src/main.cpp:2:1: error: expected ;' && exit 1";
  {
    agents::TestService svc(d.path(), cfg);
    auto r = svc.build_and_test({});
    REQUIRE(r);
    CHECK_FALSE(r->build_ok);
    CHECK(core::contains(r->text, "src/main.cpp:2:1: error: expected ;"));
  }
  cfg.build_command = "exit 3";
  {
    agents::TestService svc(d.path(), cfg);
    auto r = svc.build_and_test({});
    REQUIRE(r);
    CHECK_FALSE(r->build_ok);
    CHECK(core::contains(r->text, "build command failed (exit 3): exit 3"));
  }
}

TEST_CASE("testing: the compile check compiles the given files by compile_commands.json, writing nothing (ADR 0028)") {
  test::TempDir d{"owncc"};
  own_project(d);
  test::write(d / "src/ok.cpp", "int twice(int x) { return 2 * x; }\n");
  test::write(d / "src/warn.cpp", "int f() { int unused = 1; return 0; }\n");
  test::write(d / "src/bad.cpp", "int g() { return missing_name; }\n");
  // A multi-config build lists a file once per configuration; the release entry is the one compiled.
  std::string cc = "[\n";
  for (const char* f : {"ok", "warn", "bad"}) {
    for (const char* cfg : {"Debug", "Release"}) {
      cc += fmt::format(R"(  {{"directory": "{0}", "command": "c++ -Wall -DCFG_{2} -o obj/{2}/{1}.o -c {0}/src/{1}.cpp", "file": "{0}/src/{1}.cpp", "output": "obj/{2}/{1}.o"}},)",
                        d.path().string(), f, cfg) + "\n";
    }
  }
  cc.pop_back();
  cc.pop_back();
  cc += "\n]\n";
  test::write(d / "build/compile_commands.json", cc);
  core::Config cfg;
  agents::TestService svc(d.path(), cfg);
  auto ok = svc.compile_check({"src/ok.cpp", "src/warn.cpp"});
  REQUIRE_MESSAGE(ok, (ok ? "" : ok.error().message));
  CHECK(ok->build_ok);
  CHECK(core::contains(ok->text, "compile check ok: src/ok.cpp src/warn.cpp"));
  CHECK(core::contains(ok->build_warnings, "src/warn.cpp:1:15: warning: unused variable"));
  CHECK_FALSE(std::filesystem::exists(d / "obj"));  // -fsyntax-only: no object written
  auto bad = svc.compile_check({"src/bad.cpp", "src/not_in_build.cpp"});
  REQUIRE(bad);
  CHECK_FALSE(bad->build_ok);
  CHECK(core::contains(bad->build_summary, "src/bad.cpp:1:18: error:"));  // GCC may quote with ‘ ’
  CHECK(core::contains(bad->build_summary, "missing_name"));
  CHECK(core::contains(bad->text, "not checked: src/not_in_build.cpp"));
}

TEST_CASE("testing: a verify_command checks the agents' work instead of the release build_command (ADR 0029)") {
  test::TempDir d{"verifycmd"};
  own_project(d);
  core::Config cfg;
  cfg.build_command = "touch release-was-built && echo '[1/1] Linking app (release, LTO)'";
  cfg.verify_command = "touch debug-was-built && echo '[1/1] Linking app (debug)'";
  cfg.test_command = "true";
  agents::TestService svc(d.path(), cfg);
  auto r = svc.build_and_test({"app"});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().message));
  CHECK(r->build_ok);
  CHECK(std::filesystem::exists(d / "debug-was-built"));
  CHECK_FALSE(std::filesystem::exists(d / "release-was-built"));
  // Without it the release build is the check, as before.
  cfg.verify_command.clear();
  agents::TestService svc2(d.path(), cfg);
  REQUIRE(svc2.build_and_test({"app"}));
  CHECK(std::filesystem::exists(d / "release-was-built"));
}

TEST_CASE("testing: the test command gets the ticket's behaviour test files, and files of the test folder since its start (ADR 0029)") {
  test::TempDir d{"tickettests"};
  own_project(d);
  test::write(d / "tests/behaviour/old_test.txt", "old\n");
  core::Config cfg;
  cfg.build_command = "true";
  cfg.test_command = "printf '%s' \"$KK_TEST_FILES\" > seen.txt";
  cfg.test_dir = "tests/behaviour";
  auto since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 1;
  test::write(d / ".karkas/cache/ticket_tests.txt", fmt::format("# T-0001 since={}\ntests/behaviour/listed.txt\n", since));
  test::write(d / "tests/behaviour/listed.txt", "listed\n");
  test::write(d / "tests/behaviour/new_test.txt", "new\n");
  // new_test.txt counts as written after `since`: its time is set past it; old_test.txt stays before.
  std::filesystem::last_write_time(d / "tests/behaviour/new_test.txt",
                                   std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
  std::filesystem::last_write_time(d / "tests/behaviour/old_test.txt",
                                   std::filesystem::file_time_type::clock::now() - std::chrono::seconds(60));
  agents::TestService svc(d.path(), cfg);
  REQUIRE(svc.build_and_test({"app"}));
  auto seen = test::read(d / "seen.txt");
  CHECK(core::contains(seen, "tests/behaviour/listed.txt"));
  CHECK(core::contains(seen, "tests/behaviour/new_test.txt"));
  CHECK_FALSE(core::contains(seen, "old_test.txt"));
}

TEST_CASE("testing: doctest for target builds is the build tree's when it is there, else the copy carried by Karkas (ADR 0047)") {
  test::TempDir d{"doctest"};
  const auto embedded = templates::get("vendor/doctest/doctest.h");
  REQUIRE(embedded);
  CHECK(core::contains(*embedded, "DOCTEST_VERSION_MAJOR"));
  // A developer build: its build tree still has doctest.
  test::write(d / "tree/doctest/doctest.h", "// doctest\n");
  CHECK(agents::TestService::doctest_include((d / "tree").string(), d / "data") == (d / "tree").string());
  CHECK_FALSE(std::filesystem::exists(d / "data"));
  // A package on another machine: the folder it was built in is not there (v0.1.0 passed it anyway).
  auto dir = agents::TestService::doctest_include("/nonexistent/build/_deps/doctest-src", d / "data");
  REQUIRE_FALSE(dir.empty());
  CHECK(std::filesystem::path(dir).parent_path() == d / "data");
  CHECK(std::filesystem::path(dir).filename().string().starts_with("doctest-2."));
  CHECK(test::read(std::filesystem::path(dir) / "doctest/doctest.h") == *embedded);
  // Kept as it is; a damaged copy is written again.
  CHECK(agents::TestService::doctest_include("", d / "data") == dir);
  test::write(std::filesystem::path(dir) / "doctest/doctest.h", "broken");
  CHECK(agents::TestService::doctest_include("", d / "data") == dir);
  CHECK(test::read(std::filesystem::path(dir) / "doctest/doctest.h") == *embedded);
  // Nowhere to write it: nothing is passed, the generated CMake looks for doctest itself.
  CHECK(agents::TestService::doctest_include("", {}).empty());
}
