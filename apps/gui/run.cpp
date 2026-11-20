#include "run.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

#include "kk/core/text.hpp"

extern char** environ;

namespace kk::gui {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::vector<std::string> run_args(bool build) {
  std::vector<std::string> a = {"run"};
  if (!build) a.emplace_back("--no-build");
  return a;
}

// --- terminal text ---------------------------------------------------------------------------

void TerminalText::feed(std::string_view chunk, std::vector<std::string>& lines) {
  for (char c : chunk) {
    auto u = static_cast<unsigned char>(c);
    switch (esc_) {
      case Esc::Start:
        esc_ = c == '[' ? Esc::Csi : c == ']' ? Esc::Osc : Esc::None;  // ESC + one character otherwise
        continue;
      case Esc::Csi:
        if (u >= 0x40 && u <= 0x7e) esc_ = Esc::None;  // the final byte
        continue;
      case Esc::Osc:
        if (c == '\a') esc_ = Esc::None;
        if (c == '\x1b') esc_ = Esc::OscEsc;
        continue;
      case Esc::OscEsc:
        esc_ = c == '\\' ? Esc::None : Esc::Osc;
        continue;
      case Esc::None:
        break;
    }
    if (c == '\x1b') {
      esc_ = Esc::Start;
      continue;
    }
    if (c == '\n') {
      lines.push_back(std::exchange(line_, {}));
      cr_ = false;
      continue;
    }
    if (c == '\r') {
      cr_ = true;
      continue;
    }
    if (cr_) {
      line_.clear();
      cr_ = false;
    }
    if (c == '\b') {
      // One character back, a UTF-8 sequence as a whole.
      while (!line_.empty() && (static_cast<unsigned char>(line_.back()) & 0xC0) == 0x80) line_.pop_back();
      if (!line_.empty()) line_.pop_back();
      continue;
    }
    if (u < 0x20 && c != '\t') continue;  // bell and the other control characters
    if (u == 0x7f) continue;
    line_ += c;
    if (line_.size() >= kMaxLine && (u & 0xC0) != 0x80) lines.push_back(std::exchange(line_, {}));
  }
}

// --- session ---------------------------------------------------------------------------------

RunSession::RunSession(fs::path root, fs::path cli, std::function<void()> wake)
    : root_(std::move(root)), cli_(std::move(cli)), wake_(std::move(wake)) {}

RunSession::~RunSession() {
  if (busy()) {
    stop();
    if (!wait(std::chrono::milliseconds(static_cast<int>(kKillAfter * 1000) + 2000))) {
      std::lock_guard lk(mu_);
      if (pid_ > 0) ::kill(-pid_, SIGKILL);
    }
  }
  {
    std::lock_guard lk(mu_);
    quit_ = true;
  }
  if (thread_.joinable()) thread_.join();
}

void RunSession::add_locked(RunLine l) {
  lines_.push_back(std::move(l));
  if (lines_.size() > kMaxLines) lines_.erase(lines_.begin(), lines_.begin() + static_cast<long>(lines_.size() - kMaxLines));
  ++serial_;
}

void RunSession::line_locked(std::string text) {
  if (!text.empty() && text.back() == '\r') text.pop_back();
  RunLine l{RunLine::Kind::Out, std::move(text)};
  // Until the program starts, the lines are karkas's ("· ..." progress, "karkas: ..." errors) and the
  // build's; after it, every line is the program's own.
  if (phase_ == RunPhase::Building) {
    if (l.text.starts_with("· ")) {
      l.text.erase(0, std::string_view("· ").size());
      l.kind = RunLine::Kind::Info;
      if (l.text.starts_with("running: ")) {
        program_ = l.text.substr(std::string_view("running: ").size());
        phase_ = RunPhase::Running;
        end_.started = true;
      } else if (l.text == "build failed") {
        l.kind = RunLine::Kind::Error;
      }
    } else if (l.text.starts_with("karkas: ")) {
      l.kind = RunLine::Kind::Error;
      if (l.text.starts_with("karkas: nothing to run")) end_.nothing_to_run = true;
    } else {
      auto low = core::to_lower(l.text);
      if (core::contains(low, "error:") || core::contains(low, "undefined reference") || core::contains(low, "cmake error")) {
        l.kind = RunLine::Kind::Error;
      } else if (core::contains(low, "warning:")) {
        l.kind = RunLine::Kind::Warning;
      }
    }
  }
  add_locked(std::move(l));
}

bool RunSession::start(bool build) {
  std::unique_lock lk(mu_);
  auto fail = [&](const std::string& why) {
    add_locked({RunLine::Kind::Error, why});
    lk.unlock();
    if (wake_) wake_();
    return false;
  };
  if (phase_ != RunPhase::Idle) return fail("a run is going on: stop it first");
  if (thread_.joinable()) {
    // The previous run has ended (Idle): its reader is finishing.
    lk.unlock();
    thread_.join();
    lk.lock();
  }
  lines_.clear();
  term_ = {};
  program_.clear();
  end_ = {};
  stop_at_.reset();
  killed_ = false;
  ++serial_;
  // A terminal of its own: the program's output comes line by line (a pipe would buffer it), and
  // what is typed into the Run tab is its input.
  int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  char name[256] = {};
  if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0 || ::ptsname_r(master, name, sizeof name) != 0) {
    if (master >= 0) ::close(master);
    return fail(std::string("cannot open a terminal for the program: ") + std::strerror(errno));
  }
  winsize ws{};
  ws.ws_row = 50;
  ws.ws_col = 200;
  ::ioctl(master, TIOCSWINSZ, &ws);
  std::vector<std::string> argv_s = {cli_.string(), "--project", root_.string()};
  for (auto& a : run_args(build)) argv_s.push_back(std::move(a));
  std::vector<char*> argv;
  argv.reserve(argv_s.size() + 1);
  for (auto& a : argv_s) argv.push_back(a.data());
  argv.push_back(nullptr);
  // The Run tab shows text, not a full terminal: TERM=dumb asks for no colours and no cursor moves.
  std::vector<std::string> env_s;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    if (!std::string_view(*e).starts_with("TERM=")) env_s.emplace_back(*e);
  }
  env_s.emplace_back("TERM=dumb");
  std::vector<char*> envp;
  envp.reserve(env_s.size() + 1);
  for (auto& e : env_s) envp.push_back(e.data());
  envp.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  // Opened after setsid(): the terminal becomes the controlling terminal of the new session.
  posix_spawn_file_actions_addopen(&fa, 0, name, O_RDWR, 0);
  posix_spawn_file_actions_adddup2(&fa, 0, 1);
  posix_spawn_file_actions_adddup2(&fa, 0, 2);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  // A session (and process group) of its own: Stop ends karkas, the build and the program together.
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
  sigset_t def;
  sigemptyset(&def);
  for (int s : {SIGINT, SIGTERM, SIGPIPE, SIGHUP, SIGQUIT, SIGCHLD}) sigaddset(&def, s);
  posix_spawnattr_setsigdefault(&attr, &def);
  sigset_t mask;
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  pid_t pid = 0;
  bool path_search = cli_.string().find('/') == std::string::npos;
  int rc = path_search ? posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), envp.data())
                       : posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    ::close(master);
    return fail("cannot start " + cli_.string() + ": " + std::strerror(rc));
  }
  pid_ = pid;
  master_ = master;
  phase_ = RunPhase::Building;
  started_at_ = Clock::now();
  thread_ = std::thread([this, master, pid] { reader(master, pid); });
  lk.unlock();
  if (wake_) wake_();
  return true;
}

void RunSession::stop() {
  {
    std::lock_guard lk(mu_);
    if (phase_ == RunPhase::Idle || pid_ <= 0 || stop_at_) return;
    ::kill(-pid_, SIGTERM);
    stop_at_ = Clock::now();
    ++serial_;
  }
  if (wake_) wake_();
}

bool RunSession::send_input(const std::string& line) {
  std::lock_guard lk(mu_);
  if (phase_ == RunPhase::Idle || master_ < 0) return false;
  std::string data = line + "\n";
  std::size_t off = 0;
  while (off < data.size()) {
    auto n = ::write(master_, data.data() + off, data.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    off += static_cast<std::size_t>(n);
  }
  return true;
}

RunPhase RunSession::phase() const {
  std::lock_guard lk(mu_);
  return phase_;
}

bool RunSession::stopping() const {
  std::lock_guard lk(mu_);
  return phase_ != RunPhase::Idle && stop_at_.has_value();
}

double RunSession::elapsed() const {
  std::lock_guard lk(mu_);
  if (phase_ == RunPhase::Idle) return end_.seconds;
  return std::chrono::duration<double>(Clock::now() - started_at_).count();
}

std::string RunSession::program() const {
  std::lock_guard lk(mu_);
  return program_;
}

RunEnd RunSession::last_end() const {
  std::lock_guard lk(mu_);
  return end_;
}

std::uint64_t RunSession::serial() const {
  std::lock_guard lk(mu_);
  return serial_;
}

std::vector<RunLine> RunSession::lines() const {
  std::lock_guard lk(mu_);
  return lines_;
}

std::string RunSession::partial_line() const {
  std::lock_guard lk(mu_);
  return term_.partial();
}

void RunSession::clear() {
  std::lock_guard lk(mu_);
  if (phase_ != RunPhase::Idle) return;
  lines_.clear();
  term_ = {};
  ++serial_;
}

bool RunSession::wait(std::chrono::milliseconds timeout) {
  auto until = Clock::now() + timeout;
  while (busy()) {
    if (Clock::now() >= until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

void RunSession::reader(int master, pid_t pid) {
  char buf[8192];
  int status = 0;
  bool reaped = false;
  bool hung_up = false;  // every process closed the terminal
  auto last_wake = Clock::now();
  // Reads what is there within `timeout_ms`; false when nothing came.
  auto pump = [&](int timeout_ms) {
    if (hung_up) return false;
    pollfd p{master, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) <= 0) return false;
    auto n = ::read(master, buf, sizeof buf);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return true;
    if (n <= 0) {
      hung_up = true;  // EIO: the last process that had the terminal open closed it
      return false;
    }
    std::vector<std::string> done;
    {
      std::lock_guard lk(mu_);
      term_.feed(std::string_view(buf, static_cast<std::size_t>(n)), done);
      for (auto& l : done) line_locked(std::move(l));
      ++serial_;  // the line being written changed too
    }
    if (wake_) wake_();
    last_wake = Clock::now();
    return true;
  };
  while (true) {
    {
      std::lock_guard lk(mu_);
      if (quit_) break;
      if (stop_at_ && !killed_ && std::chrono::duration<double>(Clock::now() - *stop_at_).count() >= kKillAfter) {
        ::kill(-pid, SIGKILL);  // it did not end on SIGTERM
        killed_ = true;
      }
    }
    if (hung_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
      pump(200);
    }
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      reaped = true;
      // Take what it wrote last, even if a process it left behind still holds the terminal.
      while (pump(50)) {
      }
      break;
    }
    // The time in the Run tab moves on while the program says nothing.
    if (Clock::now() - last_wake >= std::chrono::seconds(1)) {
      last_wake = Clock::now();
      if (wake_) wake_();
    }
  }
  {
    std::lock_guard lk(mu_);
    if (!reaped && quit_) {
      ::kill(-pid, SIGKILL);
      reaped = ::waitpid(pid, &status, 0) == pid;
    }
    if (!term_.partial().empty()) line_locked(term_.take_partial());
    end_.ended = true;
    end_.stopped = stop_at_.has_value();
    end_.seconds = std::chrono::duration<double>(Clock::now() - started_at_).count();
    if (reaped && WIFEXITED(status)) end_.exit_code = WEXITSTATUS(status);
    if (reaped && WIFSIGNALED(status)) end_.signal = WTERMSIG(status);
    end_.build_failed = !end_.started && !end_.stopped && !end_.nothing_to_run;
    ::close(master);
    master_ = -1;
    pid_ = 0;
    phase_ = RunPhase::Idle;
    stop_at_.reset();
    ++serial_;
  }
  if (wake_) wake_();
}

}  // namespace kk::gui
