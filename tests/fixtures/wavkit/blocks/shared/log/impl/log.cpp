#include "log.hpp"

#include <atomic>
#include <cstdio>

namespace wk {

namespace {
std::atomic<int> g_min{static_cast<int>(Level::Info)};
}

void set_log_level(Level min_level) { g_min = static_cast<int>(min_level); }

void log(Level level, std::string_view msg) {
  if (static_cast<int>(level) < g_min) return;
  static const char* kNames[] = {"debug", "info", "warn", "error"};
  std::fprintf(stderr, "wavkit %s: %.*s\n", kNames[static_cast<int>(level)], static_cast<int>(msg.size()), msg.data());
}

}  // namespace wk
