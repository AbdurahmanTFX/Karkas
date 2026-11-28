#include "text.hpp"

#include <fmt/format.h>

#include <algorithm>

namespace kk::hsgm::detail {

std::string table(const std::vector<std::vector<std::string>>& rows) {
  std::vector<std::size_t> width;
  for (const auto& r : rows) {
    for (std::size_t i = 0; i < r.size(); ++i) {
      if (width.size() <= i) width.push_back(0);
      width[i] = std::max(width[i], r[i].size());
    }
  }
  std::string out;
  for (const auto& r : rows) {
    std::size_t last = 0;
    for (std::size_t i = 0; i < r.size(); ++i) {
      if (!r[i].empty()) last = i;
    }
    std::string line;
    for (std::size_t i = 0; i <= last && i < r.size(); ++i) {
      line += r[i];
      if (i != last) line.append(width[i] - r[i].size() + 2, ' ');
    }
    out += line + "\n";
  }
  return out;
}

std::string capped(const std::vector<std::string>& items, std::string_view sep) {
  std::string out;
  const std::size_t shown = std::min(items.size(), kListCap);
  for (std::size_t i = 0; i < shown; ++i) {
    if (i > 0) out += sep;
    out += items[i];
  }
  if (items.size() > shown) out += fmt::format("{}… ({} more)", shown > 0 ? std::string(sep) : "", items.size() - shown);
  return out;
}

}  // namespace kk::hsgm::detail
