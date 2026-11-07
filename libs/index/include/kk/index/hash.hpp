#pragma once
// xxh3 (64-bit) hashes of files and strings, as 16 hex digits (7.2).
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kk::index {

std::string xxh3_hex(std::string_view data);
// Combined hash of (path, hash) pairs, order independent of input order.
std::string combine_hashes(std::vector<std::pair<std::string, std::string>> items);

}  // namespace kk::index
