#pragma once
// Text helpers shared by the formatters of kk_hsgm (private).
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kk::hsgm::detail {

inline constexpr std::size_t kListCap = 20;

// Column-aligned rows like the views: two spaces between columns, the last non-empty cell not padded.
std::string table(const std::vector<std::vector<std::string>>& rows);

// The first kListCap items joined by `sep`, then "… (<n> more)".
std::string capped(const std::vector<std::string>& items, std::string_view sep);

}  // namespace kk::hsgm::detail
