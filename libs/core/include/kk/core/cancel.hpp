#pragma once
// Stopping agents from outside (docs/adr/0019). Two sources:
//  - the whole process: request_cancel() (the CLI calls it on SIGINT/SIGTERM; it is
//    async-signal-safe);
//  - one agent run: a CancelScope on the run's thread with a check (the stop file of the
//    run in .karkas/runs/active/).
// Long waits (HTTP requests, retry back-off) ask cancelled() and give up early.
#include <functional>

namespace kk::core {

void request_cancel() noexcept;
void reset_cancel() noexcept;  // tests, or a process that starts over
[[nodiscard]] bool cancel_requested() noexcept;  // the process-wide flag only

// The process-wide flag or any CancelScope active on this thread.
[[nodiscard]] bool cancelled();

class CancelScope {
 public:
  explicit CancelScope(std::function<bool()> check);
  ~CancelScope();
  CancelScope(const CancelScope&) = delete;
  CancelScope& operator=(const CancelScope&) = delete;

 private:
  friend bool cancelled();
  std::function<bool()> check_;
  CancelScope* prev_ = nullptr;
};

}  // namespace kk::core
