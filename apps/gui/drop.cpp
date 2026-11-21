#include "drop.hpp"

#include <mutex>
#include <utility>

namespace kk::gui {

namespace {
std::mutex g_mu;
std::vector<std::string> g_dropped;
}  // namespace

void push_dropped(std::vector<std::string> paths) {
  std::lock_guard lock{g_mu};
  for (auto& p : paths) g_dropped.push_back(std::move(p));
}

std::vector<std::string> take_dropped() {
  std::lock_guard lock{g_mu};
  return std::exchange(g_dropped, {});
}

}  // namespace kk::gui
