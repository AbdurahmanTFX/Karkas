#include "chat.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

extern char** environ;

namespace kk::gui {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Words of a command line: "double" and 'single' quotes keep spaces, \" inside double quotes.
std::vector<std::string> words(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  bool have = false;
  char quote = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (quote != 0) {
      if (c == quote) {
        quote = 0;
      } else if (quote == '"' && c == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\')) {
        cur += s[++i];
      } else {
        cur += c;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
      have = true;
    } else if (c == ' ' || c == '\t' || c == '\n') {
      if (have || !cur.empty()) out.push_back(std::move(cur));
      cur.clear();
      have = false;
    } else {
      cur += c;
    }
  }
  if (have || !cur.empty()) out.push_back(std::move(cur));
  return out;
}

const char* kind_name(ChatEntry::Kind k) {
  switch (k) {
    case ChatEntry::Kind::User: return "user";
    case ChatEntry::Kind::Progress: return "progress";
    case ChatEntry::Kind::Log: return "log";
    case ChatEntry::Kind::Result: return "result";
    case ChatEntry::Kind::Error: return "error";
    case ChatEntry::Kind::Info: return "info";
  }
  return "info";
}

ChatEntry::Kind kind_from(const std::string& s) {
  for (auto k : {ChatEntry::Kind::User, ChatEntry::Kind::Progress, ChatEntry::Kind::Log, ChatEntry::Kind::Result,
                 ChatEntry::Kind::Error}) {
    if (s == kind_name(k)) return k;
  }
  return ChatEntry::Kind::Info;
}

constexpr std::size_t kMaxEntries = 2000;

}  // namespace

std::string ChatScope::str() const {
  switch (kind) {
    case Kind::Project: return "";
    case Kind::Block: return "block " + id;
    case Kind::Ticket: return "ticket " + id;
  }
  return "";
}

std::vector<std::string> chat_command(ChatMode mode, const std::string& text, std::string* error, const ChatScope& scope) {
  auto t = std::string(core::trim(text));
  auto fail = [&](const std::string& e) {
    if (error != nullptr) *error = e;
    return std::vector<std::string>{};
  };
  if (t.empty()) return fail("empty message");
  if (t[0] != '/') {
    if (scope.kind == ChatScope::Kind::Ticket && !scope.id.empty()) return {"ticket", "run", scope.id, "--note", t};
    if (scope.kind == ChatScope::Kind::Block && !scope.id.empty()) return {mode == ChatMode::Bug ? "debug" : "task", "--block", scope.id, t};
    return {mode == ChatMode::Bug ? "debug" : "task", t};
  }
  auto w = words(std::string_view(t).substr(1));
  if (w.empty()) return fail("a command after '/', for example /ticket run T-0001");
  // Commands of the agents and the ones that look at their results; nothing that starts
  // servers, rewrites the schema by itself or runs benchmarks.
  static const std::set<std::string> allowed = {"task", "debug", "ticket", "ask",  "approve", "change", "implement",
                                                "expand", "design", "test", "check", "cost", "view"};
  if (!allowed.contains(w[0])) {
    std::string list;
    for (const auto& a : allowed) list += (list.empty() ? "" : ", ") + a;
    return fail("the chat runs these commands: " + list);
  }
  return w;
}

std::string ticket_in(const std::string& line) {
  static const std::regex re(R"((?:^|[^A-Za-z0-9_-])(T-[0-9]{4,})\b)");
  std::smatch m;
  return std::regex_search(line, m, re) ? m[1].str() : "";
}

std::vector<std::string> changes_in(const std::string& text) {
  static const std::regex re(R"((?:^|[^A-Za-z0-9_-])(C-[0-9]{4,})\b)");
  std::vector<std::string> out;
  for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
    auto id = (*it)[1].str();
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

fs::path find_karkas_cli() {
  std::error_code ec;
  auto self = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    for (const auto& cand : {self.parent_path() / "karkas", self.parent_path().parent_path() / "cli" / "karkas"}) {
      if (fs::is_regular_file(cand, ec) && ::access(cand.c_str(), X_OK) == 0) return cand;
    }
  }
  return "karkas";
}

// --- Chat -----------------------------------------------------------------------------------

Chat::Chat(fs::path root, fs::path cli, std::function<void()> wake)
    : root_(std::move(root)), cli_(std::move(cli)), file_(root_ / ".karkas" / "chat.jsonl"), wake_(std::move(wake)) {
  auto text = core::read_file(file_);
  if (!text) return;
  for (const auto& line : core::split(*text, '\n')) {
    try {
      auto j = json::parse(line);
      entries_.push_back({kind_from(j.value("kind", "")), j.value("text", ""), j.value("mode", ""), j.value("ticket", ""),
                          j.value("ts", std::int64_t{0}), j.value("scope", "")});
    } catch (const json::exception&) {
    }
  }
  if (entries_.size() > kMaxEntries) entries_.erase(entries_.begin(), entries_.end() - static_cast<long>(kMaxEntries));
  serial_ = entries_.size();
}

Chat::~Chat() {
  if (busy()) {
    stop();
    if (!wait(std::chrono::seconds(5))) {
      kill();
      (void)wait(std::chrono::seconds(2));
    }
  }
  {
    std::lock_guard lk(mu_);
    quit_ = true;
  }
  if (thread_.joinable()) thread_.join();
}

void Chat::add_locked(ChatEntry e) {
  if (e.ts == 0) e.ts = core::unix_now();
  json j = {{"kind", kind_name(e.kind)}, {"text", e.text}, {"ts", e.ts}};
  if (!e.mode.empty()) j["mode"] = e.mode;
  if (!e.ticket.empty()) j["ticket"] = e.ticket;
  if (!e.scope.empty()) j["scope"] = e.scope;
  (void)core::append_file(file_, j.dump() + "\n");
  entries_.push_back(std::move(e));
  if (entries_.size() > kMaxEntries) entries_.erase(entries_.begin());
  ++serial_;
}

void Chat::add(ChatEntry e) {
  {
    std::lock_guard lk(mu_);
    add_locked(std::move(e));
  }
  if (wake_) wake_();
}

bool Chat::busy() const {
  std::lock_guard lk(mu_);
  return busy_;
}

std::optional<double> Chat::stopping_for() const {
  std::lock_guard lk(mu_);
  if (!busy_ || !stop_at_) return std::nullopt;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - *stop_at_).count();
}

std::string Chat::current_ticket() const {
  std::lock_guard lk(mu_);
  return ticket_;
}

std::uint64_t Chat::serial() const {
  std::lock_guard lk(mu_);
  return serial_;
}

std::vector<ChatEntry> Chat::entries() const {
  std::lock_guard lk(mu_);
  return entries_;
}

void Chat::clear() {
  std::lock_guard lk(mu_);
  entries_.clear();
  std::error_code ec;
  fs::remove(file_, ec);
  ++serial_;
}

bool Chat::send(ChatMode mode, const std::string& text, const ChatScope& scope) {
  std::string error;
  auto args = chat_command(mode, text, &error, scope);
  std::unique_lock lk(mu_);
  if (busy_) {
    lk.unlock();
    add({ChatEntry::Kind::Error, "the previous command is still running", "", "", 0});
    return false;
  }
  bool command = !core::trim(text).empty() && core::trim(text)[0] == '/';
  add_locked({ChatEntry::Kind::User, std::string(core::trim(text)), command ? "command" : (mode == ChatMode::Bug ? "bug" : "task"),
              scope.kind == ChatScope::Kind::Ticket ? scope.id : "", 0, command ? "" : scope.str()});
  if (args.empty()) {
    add_locked({ChatEntry::Kind::Error, error, "", "", 0});
    lk.unlock();
    if (wake_) wake_();
    return false;
  }
  if (thread_.joinable()) {
    // The previous command has ended (busy_ is false): its reader is finishing.
    lk.unlock();
    thread_.join();
    lk.lock();
  }
  int out[2] = {-1, -1};
  int err[2] = {-1, -1};
  if (::pipe2(out, O_CLOEXEC) != 0 || ::pipe2(err, O_CLOEXEC) != 0) {
    for (int fd : {out[0], out[1], err[0], err[1]}) {
      if (fd >= 0) ::close(fd);
    }
    add_locked({ChatEntry::Kind::Error, "cannot create pipes", "", "", 0});
    return false;
  }
  std::vector<std::string> argv_s = {cli_.string(), "--project", root_.string()};
  argv_s.insert(argv_s.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : argv_s) argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, out[1], 1);
  posix_spawn_file_actions_adddup2(&fa, err[1], 2);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  // Own process group (Kill ends the command with its builds); default signal handling and
  // an empty mask whatever the GUI uses.
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
  posix_spawnattr_setpgroup(&attr, 0);
  sigset_t def;
  sigemptyset(&def);
  for (int s : {SIGINT, SIGTERM, SIGPIPE, SIGHUP, SIGQUIT}) sigaddset(&def, s);
  posix_spawnattr_setsigdefault(&attr, &def);
  sigset_t mask;
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  pid_t pid = 0;
  bool path_search = cli_.string().find('/') == std::string::npos;
  int rc = path_search ? posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), environ)
                       : posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  ::close(out[1]);
  ::close(err[1]);
  if (rc != 0) {
    ::close(out[0]);
    ::close(err[0]);
    add_locked({ChatEntry::Kind::Error, "cannot start " + cli_.string() + ": " + std::strerror(rc), "", "", 0});
    lk.unlock();
    if (wake_) wake_();
    return false;
  }
  pid_ = pid;
  busy_ = true;
  stop_at_.reset();
  ticket_.clear();
  // A command on a known ticket (/ticket run T-0003) belongs to it from the start.
  for (const auto& a : args) {
    if (auto t = ticket_in(a); !t.empty()) {
      ticket_ = t;
      break;
    }
  }
  thread_ = std::thread([this, o = out[0], e = err[0], pid] { reader(o, e, pid); });
  lk.unlock();
  if (wake_) wake_();
  return true;
}

void Chat::stop() {
  {
    std::lock_guard lk(mu_);
    if (!busy_ || pid_ <= 0 || stop_at_) return;
    ::kill(pid_, SIGINT);
    stop_at_ = std::chrono::steady_clock::now();
    add_locked({ChatEntry::Kind::Info, "stopping: the agents finish their journals, the ticket stays open", "", ticket_, 0});
  }
  if (wake_) wake_();
}

void Chat::kill() {
  {
    std::lock_guard lk(mu_);
    if (!busy_ || pid_ <= 0) return;
    ::kill(-pid_, SIGKILL);
    add_locked({ChatEntry::Kind::Info, "killed", "", ticket_, 0});
  }
  if (wake_) wake_();
}

bool Chat::wait(std::chrono::milliseconds timeout) {
  auto until = std::chrono::steady_clock::now() + timeout;
  while (busy()) {
    if (std::chrono::steady_clock::now() >= until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

void Chat::reader(int out_fd, int err_fd, pid_t pid) {
  std::string out, err_buf;
  std::vector<std::string> errors;
  auto stderr_line = [&](std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (core::trim(line).empty()) return;
    if (line.starts_with("karkas: ")) {
      errors.push_back(line.substr(8));
      return;
    }
    ChatEntry e;
    if (line.starts_with("· ")) {
      e.kind = ChatEntry::Kind::Progress;
      e.text = line.substr(std::string("· ").size());
    } else {
      // Log lines of the CLI: the time stamp says nothing in a chat that is happening now.
      static const std::regex stamp(R"(^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d(\.\d+)? )");
      e.kind = ChatEntry::Kind::Log;
      e.text = std::regex_replace(line, stamp, "");
    }
    std::lock_guard lk(mu_);
    if (e.kind == ChatEntry::Kind::Progress && e.text.starts_with("created ")) {
      if (auto t = ticket_in(e.text); !t.empty()) ticket_ = t;
    }
    e.ticket = ticket_;
    add_locked(std::move(e));
  };
  pollfd fds[2] = {{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}};
  int open_fds = 2;
  char buf[4096];
  int status = 0;
  bool reaped = false;
  bool quitting = false;
  // Reads what is there within `timeout_ms`; false when nothing came.
  auto pump = [&](int timeout_ms) {
    int n = ::poll(fds, 2, timeout_ms);
    if (n <= 0) return false;
    bool got = false;
    bool lines = false;
    for (auto& f : fds) {
      if (f.fd < 0 || (f.revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
      auto r = ::read(f.fd, buf, sizeof buf);
      if (r <= 0) {
        ::close(f.fd);
        f.fd = -1;
        --open_fds;
        continue;
      }
      got = true;
      if (f.fd == out_fd) {
        out.append(buf, static_cast<std::size_t>(r));
      } else {
        err_buf.append(buf, static_cast<std::size_t>(r));
        for (auto nl = err_buf.find('\n'); nl != std::string::npos; nl = err_buf.find('\n')) {
          stderr_line(err_buf.substr(0, nl));
          err_buf.erase(0, nl + 1);
          lines = true;
        }
      }
    }
    if (lines && wake_) wake_();
    return got;
  };
  while (open_fds > 0) {
    {
      std::lock_guard lk(mu_);
      quitting = quit_;
    }
    if (quitting) break;
    pump(200);
    // The command itself has ended: take what it wrote and stop, even if a process it left
    // behind still holds the pipes.
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      reaped = true;
      while (open_fds > 0 && pump(50)) {
      }
      break;
    }
  }
  for (auto& f : fds) {
    if (f.fd >= 0) ::close(f.fd);
  }
  if (!err_buf.empty()) stderr_line(err_buf);
  if (!reaped && !quitting) reaped = ::waitpid(pid, &status, 0) == pid;
  {
    std::lock_guard lk(mu_);
    pid_ = 0;
    std::string ticket = ticket_;
    if (auto t = ticket_in(out); !t.empty()) ticket = t;
    bool stopped = stop_at_.has_value() || (reaped && WIFSIGNALED(status)) || (reaped && WIFEXITED(status) && WEXITSTATUS(status) == 130);
    bool ok = reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!core::trim(out).empty()) add_locked({ChatEntry::Kind::Result, out, "", ticket, 0});
    std::string why = core::join(errors, "\n");
    if (stopped) {
      add_locked({ChatEntry::Kind::Info, why.empty() ? "stopped" : why, "", ticket, 0});
    } else if (!ok) {
      if (why.empty()) why = reaped && WIFEXITED(status) ? "the command failed with exit code " + std::to_string(WEXITSTATUS(status)) : "the command failed";
      add_locked({ChatEntry::Kind::Error, why, "", ticket, 0});
    } else if (core::trim(out).empty()) {
      add_locked({ChatEntry::Kind::Info, "done", "", ticket, 0});
    }
    ticket_ = ticket;
    busy_ = false;
    stop_at_.reset();
  }
  if (wake_) wake_();
}

}  // namespace kk::gui
