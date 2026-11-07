#pragma once
// Minimal logging to stderr (shared.log).
#include <string_view>

namespace wk {

enum class Level { Debug, Info, Warn, Error };

void log(Level level, std::string_view msg);
void set_log_level(Level min_level);

}  // namespace wk
