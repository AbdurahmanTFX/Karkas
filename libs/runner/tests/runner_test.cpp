#include <doctest/doctest.h>

#include <cstdlib>

#include "kk/core/text.hpp"
#include "kk/runner/runner.hpp"
#include "kk/test/support.hpp"

using namespace kk;
using namespace std::chrono_literals;

TEST_CASE("runner: runs a process and captures merged output") {
  auto r = runner::run({{"sh", "-c", "echo out; echo err 1>&2; exit 3"}, {}, 10s});
  REQUIRE(r);
  CHECK(r->exit_code == 3);
  CHECK_FALSE(r->timed_out);
  CHECK(core::contains(r->output, "out"));
  CHECK(core::contains(r->output, "err"));
}

TEST_CASE("runner: secrets never reach child processes (9.5)") {
  ::setenv("ANTHROPIC_API_KEY", "sk-should-not-leak", 1);
  ::setenv("OPENAI_API_KEY", "sk-other", 1);
  ::setenv("GITHUB_TOKEN", "ghp_x", 1);
  auto r = runner::run({{"env"}, {}, 10s, {{"KARKAS_PROBE_DIR", "/tmp/p"}, {"MY_SECRET", "x"}}});
  REQUIRE(r);
  CHECK_FALSE(core::contains(r->output, "sk-should-not-leak"));
  CHECK_FALSE(core::contains(r->output, "ANTHROPIC_API_KEY"));
  CHECK_FALSE(core::contains(r->output, "GITHUB_TOKEN"));
  CHECK_FALSE(core::contains(r->output, "MY_SECRET"));
  CHECK(core::contains(r->output, "KARKAS_PROBE_DIR=/tmp/p"));
  CHECK(core::contains(r->output, "PATH="));
  ::unsetenv("ANTHROPIC_API_KEY");
  ::unsetenv("OPENAI_API_KEY");
  ::unsetenv("GITHUB_TOKEN");
}

TEST_CASE("runner: a process past its timeout is killed") {
  auto t0 = std::chrono::steady_clock::now();
  auto r = runner::run({{"sleep", "30"}, {}, 300ms});
  REQUIRE(r);
  CHECK(r->timed_out);
  CHECK_FALSE(r->ok());
  CHECK(std::chrono::steady_clock::now() - t0 < 10s);
}

TEST_CASE("runner: build log compression keeps errors only, deduplicated, at most 40 lines") {
  std::string log = "[1/9] Building CXX object x.o\n";
  for (int i = 0; i < 60; ++i) log += "/p/blocks/a/impl/a.cpp:" + std::to_string(i) + ":3: error: 'foo' was not declared\n";
  log += "/p/blocks/a/impl/a.cpp:1:3: error: 'foo' was not declared\n";
  log += "note: something\nwarning: unused\n";
  auto s = runner::compress_build_log(log, "/p");
  auto lines = core::split(s, '\n');
  CHECK(lines.size() == 41);
  CHECK(lines[0] == "blocks/a/impl/a.cpp:0:3: error: 'foo' was not declared");
  CHECK(lines.back() == "... 20 more error lines");
  CHECK_FALSE(core::contains(s, "warning"));
}

TEST_CASE("runner: warning compression keeps compiler warnings only, deduplicated, paths relative to the root") {
  std::string log = "[1/3] Building CXX object x.o\n"
                    "/p/source/Engine.cpp:744:9: warning: unused variable 'x' [-Wunused-variable]\n"
                    "/p/source/Engine.cpp:744:9: warning: unused variable 'x' [-Wunused-variable]\n"
                    "/p/source/Engine.cpp:30:1: error: 'y' was not declared\n"
                    "warning: unused\n";
  auto w = runner::compress_build_warnings(log, "/p");
  CHECK(w == "source/Engine.cpp:744:9: warning: unused variable 'x' [-Wunused-variable]");
  auto r = runner::build_command("echo '/x/a.cpp:1:1: warning: w'; exit 2", std::filesystem::temp_directory_path(),
                                 std::chrono::seconds(10));
  REQUIRE(r);
  CHECK_FALSE(r->ok);
  CHECK(r->warnings == "/x/a.cpp:1:1: warning: w");
  CHECK(core::contains(r->summary, "exit 2"));
}

TEST_CASE("runner: a CMake error keeps its message, not only where it was raised") {
  std::string log =
      "-- Configuring done\n"
      "CMake Error at CMakeLists.txt:149 (add_executable):\n"
      "  No SOURCES given to target: sim_pickups_tests\n"
      "\n"
      "\n"
      "CMake Error at /p/CMakeLists.txt:213 (add_executable):\n"
      "  Cannot find source file:\n"
      "\n"
      "    blocks/view/hud/tests/unit/x.cpp\n"
      "-- Generating done\n";
  auto s = runner::compress_build_log(log, "/p");
  auto lines = core::split(s, '\n');
  REQUIRE(lines.size() == 2);
  CHECK(lines[0] == "CMake Error at CMakeLists.txt:149 (add_executable): No SOURCES given to target: sim_pickups_tests");
  CHECK(lines[1] == "CMake Error at CMakeLists.txt:213 (add_executable): Cannot find source file: blocks/view/hud/tests/unit/x.cpp");
}

TEST_CASE("runner: ctest -V output is parsed into per-block counts and failures") {
  std::string out = R"(test 1
    Start 1: stream.ring/contract

1: Test command: /b/stream_ring_tests "-sf=*/tests/contract/*"
1: [doctest] doctest version is "2.4.12"
1: ===============================================================================
1: /p/blocks/stream/ring/tests/contract/ring_contract.cpp:20:
1: TEST CASE:  never loses or duplicates frames, including at wraparound
1:
1: /p/blocks/stream/ring/tests/contract/ring_contract.cpp:31: ERROR: CHECK( got == sent ) is NOT correct!
1:   values: CHECK( 37 == 0 )
1:
1: [doctest] test cases: 3 | 2 passed | 1 failed | 0 skipped
1/3 Test #1: stream.ring/contract .............***Failed    0.01 sec
test 2
    Start 2: stream.ring/unit
2: [doctest] test cases: 4 | 4 passed | 0 failed | 0 skipped
2/3 Test #2: stream.ring/unit .................   Passed    0.01 sec
test 3
    Start 3: dsp/integration
3: Could not find executable /b/dsp_integration
3/3 Test #3: dsp/integration ..................***Not Run   0.00 sec
)";
  auto blocks = runner::parse_ctest_output(out, "/p");
  REQUIRE(blocks.size() == 2);
  const auto& dsp = blocks[0];
  CHECK(dsp.block == "dsp");
  CHECK(dsp.build_failed);
  CHECK(dsp.integration.total == 1);
  CHECK(dsp.integration.pass == 0);
  const auto& ring = blocks[1];
  CHECK(ring.contract.pass == 2);
  CHECK(ring.contract.total == 3);
  CHECK(ring.unit.pass == 4);
  REQUIRE(ring.failures.size() == 1);
  CHECK(ring.failures[0] ==
        "contract: never loses or duplicates frames, including at wraparound (blocks/stream/ring/tests/contract/ring_contract.cpp:31)");
  auto text = runner::summarize(blocks);
  CHECK(core::contains(text, "tests stream.ring contract 2/3 unit 4/4"));
}

TEST_CASE("runner: an inherited environment can drop named variables") {
  ::setenv("KK_TEST_DROP", "drop-me", 1);
  ::setenv("KK_TEST_KEEP", "keep-me", 1);
  runner::RunOptions o;
  o.argv = {"env"};
  o.timeout = 10s;
  o.clean_env = false;
  o.unset_env = {"KK_TEST_DROP"};
  o.env = {{"KK_TEST_KEEP", "override"}};
  auto r = runner::run(o);
  REQUIRE(r);
  CHECK_FALSE(core::contains(r->output, "drop-me"));
  CHECK(core::contains(r->output, "KK_TEST_KEEP=override"));
  CHECK_FALSE(core::contains(r->output, "keep-me"));
  CHECK(core::contains(r->output, "PATH="));
  ::unsetenv("KK_TEST_DROP");
  ::unsetenv("KK_TEST_KEEP");
}
