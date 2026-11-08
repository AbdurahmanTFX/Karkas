#include <doctest/doctest.h>

#include "kk/agents/testing.hpp"
#include "kk/core/text.hpp"
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
