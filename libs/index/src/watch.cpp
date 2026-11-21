#include "kk/index/watch.hpp"

#include <fmt/format.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

#include <array>
#include <map>
#include <memory>
#include <vector>
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

#ifdef _WIN32

// Windows: like the Linux watcher, a watch per folder (skipping the same folders), each a ReadDirectoryChangesW
// on its own; their completions arrive at one port. (One recursive watch would do on Windows, but wine reports
// no changes inside subfolders that way.)
namespace {
struct DirWatch {
  HANDLE handle = INVALID_HANDLE_VALUE;
  OVERLAPPED ov{};
  alignas(DWORD) std::array<char, 16384> buf{};
  std::filesystem::path dir;
  bool armed = false;
};
struct WinState {
  HANDLE port = nullptr;
  std::vector<std::unique_ptr<DirWatch>> dirs;
};
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                          FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;
bool arm(DirWatch& d) {
  d.ov = OVERLAPPED{};
  d.armed = ::ReadDirectoryChangesW(d.handle, d.buf.data(), static_cast<DWORD>(d.buf.size()), FALSE, kFilter, nullptr, &d.ov, nullptr) != 0;
  return d.armed;
}
}  // namespace

void Watcher::add_tree(const std::filesystem::path& dir) {
  auto* st = static_cast<WinState*>(dir_handle_);
  if (st == nullptr) return;
  auto d = std::make_unique<DirWatch>();
  d->dir = dir;
  d->handle = ::CreateFileW(dir.wstring().c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (d->handle != INVALID_HANDLE_VALUE &&
      ::CreateIoCompletionPort(d->handle, st->port, reinterpret_cast<ULONG_PTR>(d.get()), 0) != nullptr && arm(*d)) {
    st->dirs.push_back(std::move(d));
  } else if (d->handle != INVALID_HANDLE_VALUE) {
    ::CloseHandle(d->handle);
  }
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_directory(ec) && !e.is_symlink(ec) && !skip(e.path())) add_tree(e.path());
  }
}

kk::VoidResult Watcher::start(const std::filesystem::path& root, Callback cb, std::chrono::milliseconds debounce) {
  stop();
  auto* st = new WinState;
  st->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (st->port == nullptr) {
    delete st;
    return kk::fail("watch", "cannot make a completion port");
  }
  dir_handle_ = st;
  root_ = root;
  cb_ = std::move(cb);
  debounce_ = debounce;
  add_tree(root_);
  running_ = true;
  thread_ = std::thread([this] { loop(); });
  return {};
}

void Watcher::stop() {
  auto* st = static_cast<WinState*>(dir_handle_);
  if (running_) {
    running_ = false;
    if (st != nullptr) ::PostQueuedCompletionStatus(st->port, 0, 0, nullptr);
    if (thread_.joinable()) thread_.join();
  }
  if (st == nullptr) return;
  for (auto& d : st->dirs) {
    if (d->armed) {
      ::CancelIoEx(d->handle, &d->ov);
      DWORD n = 0;
      ::GetOverlappedResult(d->handle, &d->ov, &n, TRUE);  // the buffer is free only after this
    }
    ::CloseHandle(d->handle);
  }
  ::CloseHandle(st->port);
  delete st;
  dir_handle_ = nullptr;
}

void Watcher::loop() {
  auto* st = static_cast<WinState*>(dir_handle_);
  bool pending = false;
  while (running_) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* ov = nullptr;
    BOOL ok = ::GetQueuedCompletionStatus(st->port, &bytes, &key, &ov, pending ? static_cast<DWORD>(debounce_.count()) : INFINITE);
    if (!running_) break;
    if (ov == nullptr) {
      if (!ok && ::GetLastError() == WAIT_TIMEOUT) {
        pending = false;
        if (cb_) cb_();
        continue;
      }
      if (key == 0) break;  // stop()
      continue;
    }
    auto* d = reinterpret_cast<DirWatch*>(key);
    d->armed = false;
    if (!ok) continue;  // the folder is gone (or the watch was cancelled): not armed again
    if (bytes == 0) {
      pending = true;  // more changes than the buffer held
    } else {
      for (DWORD off = 0;;) {
        auto* fi = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(d->buf.data() + off);
        std::filesystem::path name(std::wstring(fi->FileName, fi->FileNameLength / sizeof(wchar_t)));
        auto n = name.string();
        if (!(n.starts_with("index.db") || n == "karkas.log" || n.find(".tmp") != std::string::npos)) {
          std::error_code ec;
          if ((fi->Action == FILE_ACTION_ADDED || fi->Action == FILE_ACTION_RENAMED_NEW_NAME) &&
              std::filesystem::is_directory(d->dir / name, ec) && !skip(d->dir / name)) {
            add_tree(d->dir / name);
          }
          pending = true;
        }
        if (fi->NextEntryOffset == 0) break;
        off += fi->NextEntryOffset;
      }
    }
    arm(*d);
  }
}

#else

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

#endif

}  // namespace kk::index
