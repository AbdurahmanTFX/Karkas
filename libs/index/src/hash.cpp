#include "kk/index/hash.hpp"

#include <fmt/format.h>
#include <xxhash.h>

#include <algorithm>

namespace kk::index {

std::string xxh3_hex(std::string_view data) {
  return fmt::format("{:016x}", static_cast<unsigned long long>(XXH3_64bits(data.data(), data.size())));
}

std::string combine_hashes(std::vector<std::pair<std::string, std::string>> items) {
  std::sort(items.begin(), items.end());
  std::string buf;
  for (const auto& [k, v] : items) {
    buf += k;
    buf += '\0';
    buf += v;
    buf += '\n';
  }
  return xxh3_hex(buf);
}

}  // namespace kk::index
