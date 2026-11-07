#include "kk/index/watch.hpp"

#include <fmt/format.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <map>
#include <mutex>
#include <string>

namespace kk::index {

namespace {
std::mutex g_mu;
std::map<int, std::filesystem::path> g_dirs;  // watch descriptor → directory (per process)

bool skip(const std::filesystem::path& p) {
  auto n = p.filename().string();
  return n.starts_with("build") || n == ".git" || n == "_deps" || n == "node_modules" || n == "cache";
}
}  // namespace

Watcher::~Watcher() { stop(); }

void Watcher::add_tree(const std::filesystem::path& dir) {
  std::error_code ec;
  constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;
  int wd = inotify_add_watch(fd_, dir.c_str(), kMask);
  if (wd >= 0) {
    std::lock_guard lk(g_mu);
    g_dirs[wd] = dir;
  }
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_directory(ec) && !e.is_symlink(ec) && !skip(e.path())) add_tree(e.path());
  }
}

kk::VoidResult Watcher::start(const std::filesystem::path& root, Callback cb, std::chrono::milliseconds debounce) {
  stop();
  fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd_ < 0) return kk::fail("watch", "inotify_init1 failed");
  if (pipe(wake_) != 0) return kk::fail("watch", "pipe failed");
  root_ = root;
  cb_ = std::move(cb);
  debounce_ = debounce;
  add_tree(root_);
  running_ = true;
  thread_ = std::thread([this] { loop(); });
  return {};
}

void Watcher::stop() {
  if (running_) {
    running_ = false;
    if (wake_[1] >= 0) (void)!write(wake_[1], "x", 1);
    if (thread_.joinable()) thread_.join();
  }
  for (int* f : {&fd_, &wake_[0], &wake_[1]}) {
    if (*f >= 0) close(*f);
    *f = -1;
  }
}

void Watcher::loop() {
  std::array<char, 16384> buf{};
  bool pending = false;
  while (running_) {
    std::array<pollfd, 2> fds{{{fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}}};
    int timeout = pending ? static_cast<int>(debounce_.count()) : -1;
    int n = poll(fds.data(), fds.size(), timeout);
    if (!running_) break;
    if (n == 0) {
      pending = false;
      if (cb_) cb_();
      continue;
    }
    if (n < 0) continue;
    if ((fds[0].revents & POLLIN) != 0) {
      ssize_t len = 0;
      while ((len = read(fd_, buf.data(), buf.size())) > 0) {
        for (ssize_t off = 0; off < len;) {
          auto* ev = reinterpret_cast<inotify_event*>(buf.data() + off);
          std::string name = ev->len > 0 ? std::string(ev->name) : "";
          off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
          if (name.starts_with("index.db") || name == "karkas.log" || name.ends_with(".tmp") ||
              name.find(".tmp") != std::string::npos) {
            continue;
          }
          if ((ev->mask & IN_ISDIR) != 0 && (ev->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
            std::filesystem::path dir;
            {
              std::lock_guard lk(g_mu);
              dir = g_dirs[ev->wd];
            }
            if (!dir.empty() && !skip(dir / name)) add_tree(dir / name);
          }
          pending = true;
        }
      }
    }
  }
}

}  // namespace kk::index
