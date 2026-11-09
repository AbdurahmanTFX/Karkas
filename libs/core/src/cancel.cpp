#include "kk/core/cancel.hpp"

#include <atomic>

namespace kk::core {

namespace {
std::atomic<bool> g_cancel{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the flag is set from a signal handler");
thread_local CancelScope* t_scope = nullptr;
}  // namespace

void request_cancel() noexcept { g_cancel.store(true, std::memory_order_relaxed); }
void reset_cancel() noexcept { g_cancel.store(false, std::memory_order_relaxed); }
bool cancel_requested() noexcept { return g_cancel.load(std::memory_order_relaxed); }

bool cancelled() {
  if (cancel_requested()) return true;
  for (auto* s = t_scope; s != nullptr; s = s->prev_) {
    if (s->check_ && s->check_()) return true;
  }
  return false;
}

CancelScope::CancelScope(std::function<bool()> check) : check_(std::move(check)), prev_(t_scope) { t_scope = this; }

CancelScope::~CancelScope() { t_scope = prev_; }

}  // namespace kk::core
