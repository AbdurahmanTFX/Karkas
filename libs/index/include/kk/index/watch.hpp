#pragma once
// File watching with inotify (6.3, 14.8; Windows: ReadDirectoryChangesW, docs/adr/0046): fires after
// changes to cards, code or .karkas/ settle for `debounce` (no polling, ~0% CPU while idle).
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "kk/core/error.hpp"

namespace kk::index {

class Watcher {
 public:
  using Callback = std::function<void()>;
  Watcher() = default;
  ~Watcher();
  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;

  // Watches `root` recursively (skipping build*, .git, .karkas/index.db*). Starts a
  // background thread that calls `cb` (from that thread).
  kk::VoidResult start(const std::filesystem::path& root, Callback cb,
                       std::chrono::milliseconds debounce = std::chrono::milliseconds(150));
  void stop();
  [[nodiscard]] bool running() const { return running_; }

 private:
  void loop();
  void add_tree(const std::filesystem::path& dir);
  int fd_ = -1;
  int wake_[2] = {-1, -1};
  void* dir_handle_ = nullptr;  // Windows: the watched directory and the event that stops the thread
  void* stop_event_ = nullptr;
  std::filesystem::path root_;
  Callback cb_;
  std::chrono::milliseconds debounce_{150};
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace kk::index
