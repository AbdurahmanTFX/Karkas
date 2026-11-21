// "Build and run" in karkas-gui (docs/adr/0037): a fake karkas stands in for `karkas run` — the
// build's lines, the program's lines in a terminal of its own, input, exit codes, Stop.
#include <doctest/doctest.h>

#include <sys/stat.h>

#include <algorithm>
#include <csignal>
#include <thread>

#include "kk/core/paths.hpp"
#include "kk/test/support.hpp"
#include "run.hpp"

using namespace kk;
using gui::RunLine;
using gui::RunPhase;

namespace {

#ifndef _WIN32  // the fake karkas is a shell script (docs/adr/0046)
std::filesystem::path fake_cli(const test::TempDir& d, const std::string& body) {
  auto p = d.path() / "karkas";
  REQUIRE(core::write_file(p, "#!/bin/sh\n" + body));
  ::chmod(p.c_str(), 0755);
  return p;
}
#endif

std::vector<std::string> texts(const std::vector<RunLine>& lines, RunLine::Kind k) {
  std::vector<std::string> out;
  for (const auto& l : lines) {
    if (l.kind == k) out.push_back(l.text);
  }
  return out;
}

// Waits until `pred` holds (the reader thread fills the session); false after 5 s.
template <typename F>
bool eventually(F pred) {
  for (int i = 0; i < 250; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

}  // namespace

TEST_CASE("run: terminal text becomes lines (escapes, carriage returns, backspace, cut pieces)") {
  gui::TerminalText t;
  std::vector<std::string> lines;
  t.feed("plain\r\n\x1b[1;32mgreen\x1b[0m\n", lines);
  CHECK(lines == std::vector<std::string>{"plain", "green"});
  lines.clear();
  t.feed("10%\r50%\r100%\n", lines);  // a progress bar keeps its last state
  CHECK(lines == std::vector<std::string>{"100%"});
  lines.clear();
  t.feed("ab\x1b[", lines);  // a sequence cut between two pieces
  t.feed("31mc\x1b]0;title\a", lines);
  t.feed("d\bé\b!\x07\n", lines);
  CHECK(lines == std::vector<std::string>{"abc!"});
  lines.clear();
  t.feed("prompt> ", lines);
  CHECK(lines.empty());
  CHECK(t.partial() == "prompt> ");
  CHECK(t.take_partial() == "prompt> ");
  CHECK(t.partial().empty());
  t.feed("a\tb\n", lines);
  CHECK(lines == std::vector<std::string>{"a       b"});
  lines.clear();
  t.feed(std::string(gui::TerminalText::kMaxLine + 10, 'x'), lines);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].size() == gui::TerminalText::kMaxLine);
}

#ifndef _WIN32
TEST_CASE("run: the build's lines, then the program's in a terminal of its own, its input and exit code") {
  test::TempDir d{"gui_run"};
  auto cli = fake_cli(d, R"(echo "args: $*"
echo "· building"
echo "src/a.cpp:3: warning: unused x"
echo "[1/2] Building CXX object a.o"
echo "· built in 0.1 s"
echo "· running: build/app"
[ -t 0 ] && echo "stdin is a terminal"
echo "TERM=$TERM"
printf 'name? '
read name
echo "hello, $name"
echo "· not a karkas line now"
exit 3
)");
  int wakes = 0;
  gui::RunSession run(d.path(), cli, [&] { ++wakes; });
  CHECK(run.phase() == RunPhase::Idle);
  REQUIRE(run.start(true));
  CHECK(run.busy());
  REQUIRE(eventually([&] { return run.partial_line() == "name? "; }));
  CHECK(run.phase() == RunPhase::Running);
  CHECK(run.program() == "build/app");
  CHECK_FALSE(run.start(true));  // one run at a time
  CHECK(run.send_input("Ada"));
  REQUIRE(run.wait(std::chrono::seconds(5)));
  CHECK(run.phase() == RunPhase::Idle);
  CHECK_FALSE(run.send_input("late"));
  auto lines = run.lines();
  auto out = texts(lines, RunLine::Kind::Out);
  CHECK(out.front() == "args: --project " + d.path().string() + " run");
  CHECK(std::find(out.begin(), out.end(), "stdin is a terminal") != out.end());
  CHECK(std::find(out.begin(), out.end(), "TERM=dumb") != out.end());
  CHECK(std::find(out.begin(), out.end(), "hello, Ada") != out.end());
  CHECK(std::find(out.begin(), out.end(), "· not a karkas line now") != out.end());  // the program's own
  CHECK(texts(lines, RunLine::Kind::Info) == std::vector<std::string>{"building", "built in 0.1 s", "running: build/app"});
  CHECK(texts(lines, RunLine::Kind::Warning) == std::vector<std::string>{"src/a.cpp:3: warning: unused x"});
  auto e = run.last_end();
  CHECK(e.ended);
  CHECK(e.started);
  CHECK_FALSE(e.build_failed);
  CHECK_FALSE(e.stopped);
  CHECK(e.exit_code == 3);
  CHECK(e.signal == 0);
  CHECK(e.seconds > 0);
  CHECK(wakes > 0);
  // Run again without the build: the old lines go.
  REQUIRE(run.start(false));
  REQUIRE(eventually([&] { return run.partial_line() == "name? "; }));
  REQUIRE(run.send_input("Bob"));
  REQUIRE(run.wait(std::chrono::seconds(5)));
  lines = run.lines();
  out = texts(lines, RunLine::Kind::Out);
  CHECK(out.front() == "args: --project " + d.path().string() + " run --no-build");
  CHECK(std::find(out.begin(), out.end(), "name? Bob") != out.end());  // the terminal echoes what is typed
  CHECK(std::find(out.begin(), out.end(), "hello, Ada") == out.end());
}
#endif

#ifndef _WIN32
TEST_CASE("run: a failed build and nothing to run end before the program") {
  test::TempDir d{"gui_run_fail"};
  auto cli = fake_cli(d, R"(echo "· building"
echo "src/a.cpp:3:1: error: expected ';'"
echo "· build failed"
echo "karkas: build failed"
exit 1
)");
  gui::RunSession run(d.path(), cli, {});
  REQUIRE(run.start(true));
  REQUIRE(run.wait(std::chrono::seconds(5)));
  auto e = run.last_end();
  CHECK(e.build_failed);
  CHECK_FALSE(e.started);
  CHECK(e.exit_code == 1);
  CHECK(texts(run.lines(), RunLine::Kind::Error) ==
        std::vector<std::string>{"src/a.cpp:3:1: error: expected ';'", "build failed", "karkas: build failed"});

  auto none = fake_cli(d, "echo 'karkas: nothing to run: the project has its own build'\nexit 1\n");
  gui::RunSession run2(d.path(), none, {});
  REQUIRE(run2.start(true));
  REQUIRE(run2.wait(std::chrono::seconds(5)));
  CHECK(run2.last_end().nothing_to_run);
  CHECK_FALSE(run2.last_end().build_failed);
}
#endif

#ifndef _WIN32
TEST_CASE("run: Stop ends the program and what it started, a program that ignores it is killed") {
  test::TempDir d{"gui_run_stop"};
  auto cli = fake_cli(d, "echo '· running: build/app'\nsleep 30 &\nwait\n");
  gui::RunSession run(d.path(), cli, {});
  REQUIRE(run.start(true));
  REQUIRE(eventually([&] { return run.phase() == RunPhase::Running; }));
  run.stop();
  CHECK(run.stopping());
  REQUIRE(run.wait(std::chrono::seconds(2)));
  auto e = run.last_end();
  CHECK(e.stopped);
  CHECK(e.signal == SIGTERM);
  CHECK_FALSE(e.build_failed);

  auto stubborn = fake_cli(d, "trap '' TERM\necho '· running: build/app'\nwhile :; do sleep 0.1; done\n");
  gui::RunSession run2(d.path(), stubborn, {});
  REQUIRE(run2.start(true));
  REQUIRE(eventually([&] { return run2.phase() == RunPhase::Running; }));
  auto t0 = std::chrono::steady_clock::now();
  run2.stop();
  REQUIRE(run2.wait(std::chrono::seconds(8)));
  CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(static_cast<int>(gui::RunSession::kKillAfter * 900)));
  CHECK(run2.last_end().signal == SIGKILL);
  CHECK(run2.last_end().stopped);
}
#endif

#ifndef _WIN32
TEST_CASE("run: closing the GUI ends a running program") {
  test::TempDir d{"gui_run_quit"};
  auto marker = d.path() / "still_running";
  auto cli = fake_cli(d, "echo '· running: build/app'\nwhile :; do touch '" + marker.string() + "'; sleep 0.1; done\n");
  {
    gui::RunSession run(d.path(), cli, {});
    REQUIRE(run.start(true));
    REQUIRE(eventually([&] { return std::filesystem::exists(marker); }));
  }
  std::filesystem::remove(marker);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK_FALSE(std::filesystem::exists(marker));
}
#endif
