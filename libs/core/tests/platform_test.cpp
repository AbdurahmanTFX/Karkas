// What differs between Linux and Windows (docs/adr/0046): the same behaviour on both. The Windows child
// processes are tested where they exist (the Windows build, run under wine).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "kk/core/paths.hpp"
#include "kk/core/platform.hpp"
#include "kk/test/support.hpp"
#ifdef _WIN32
#include "kk/core/win_process.hpp"
#endif

using namespace kk;

TEST_CASE("platform: UTC time both ways, this process alive, a file made once, an exclusive lock") {
  std::time_t t = 1700000000;  // 2023-11-14 22:13:20 UTC
  auto tm = core::utc_tm(t);
  CHECK(tm.tm_year == 123);
  CHECK(tm.tm_mon == 10);
  CHECK(tm.tm_mday == 14);
  CHECK(tm.tm_hour == 22);
  CHECK(core::utc_time(tm) == t);
  CHECK(core::pid_alive(core::current_pid()));
  CHECK_FALSE(core::pid_alive(0));

  test::TempDir d{"platform"};
  auto f = d.path() / "once.lock";
  CHECK(core::create_exclusive(f, "1 me\n"));
  CHECK_FALSE(core::create_exclusive(f));
  CHECK(core::read_file(f).value_or("") == "1 me\n");
  {
    core::FileLock lock(d.path() / "build.lock");
    CHECK(lock.locked());
  }
  core::FileLock again(d.path() / "build.lock");
  CHECK(again.locked());
}

TEST_CASE("platform: the environment, a variable set and unset, a program found in PATH") {
  core::set_env("KK_PLATFORM_TEST", "yes");
  auto env = core::environment();
  CHECK(std::ranges::find(env, std::string("KK_PLATFORM_TEST=yes")) != env.end());
  core::unset_env("KK_PLATFORM_TEST");
  env = core::environment();
  CHECK(std::ranges::none_of(env, [](const std::string& e) { return e.starts_with("KK_PLATFORM_TEST=yes"); }));
#ifdef _WIN32
  auto cmd = core::find_in_path("cmd");
  CHECK(core::is_executable(cmd));
#else
  auto sh = core::find_in_path("sh");
  CHECK(sh.find('/') != std::string::npos);
  CHECK(core::is_executable(sh));
#endif
  CHECK(core::find_in_path("kk-no-such-program") == "kk-no-such-program");
  CHECK_FALSE(core::config_home().empty());
}

#ifdef _WIN32
TEST_CASE("win process: arguments quoted as the runtime splits them again") {
  using core::win::quote_arg;
  CHECK(quote_arg(L"plain") == L"plain");
  CHECK(quote_arg(L"") == L"\"\"");
  CHECK(quote_arg(L"two words") == L"\"two words\"");
  CHECK(quote_arg(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
  CHECK(quote_arg(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
  CHECK(core::win::narrow(core::win::widen("Каркас")) == "Каркас");
}

TEST_CASE("win process: output and errors through pipes, the exit code, input, a tree ended") {
  auto collect = [](core::win::Child& c, bool err) {
    std::string out;
    char buf[256];
    for (int i = 0; i < 400; ++i) {
      long n = err ? c.read_err(buf, sizeof buf) : c.read_out(buf, sizeof buf);
      if (n < 0) break;
      if (n > 0) out.append(buf, static_cast<std::size_t>(n));
      else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return out;
  };
  core::win::SpawnSpec sp;
  sp.argv = {"cmd.exe", "/d", "/c", "echo out-line& echo err-line 1>&2& exit 3"};
  sp.command_line = "cmd.exe /d /c \"echo out-line& echo err-line 1>&2& exit 3\"";
  auto c = core::win::Child::spawn(sp);
  REQUIRE(c);
  CHECK(collect(*c, false).find("out-line") != std::string::npos);
  CHECK(collect(*c, true).find("err-line") != std::string::npos);
  CHECK(c->wait(10000) == 3);

  core::win::SpawnSpec waits;
  waits.argv = {"cmd.exe", "/d", "/c", "set /p x= & echo got %x%"};
  waits.command_line = "cmd.exe /d /v:on /c \"set /p x= & echo got !x!\"";
  waits.in = core::win::SpawnSpec::Io::Pipe;
  auto w = core::win::Child::spawn(waits);
  REQUIRE(w);
  CHECK_FALSE(w->try_wait());
  REQUIRE(w->write_in("hello\r\n"));
  w->close_in();
  CHECK(collect(*w, false).find("got hello") != std::string::npos);
  CHECK(w->wait(10000).has_value());

  core::win::SpawnSpec stuck = waits;
  auto s = core::win::Child::spawn(stuck);  // waits for input that never comes
  REQUIRE(s);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK_FALSE(s->try_wait());
  s->kill_tree(137);
  CHECK(s->wait(10000) == 137);
}
#endif
