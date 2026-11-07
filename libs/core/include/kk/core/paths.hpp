#pragma once
// Path and file utilities.
#include <filesystem>
#include <string>
#include <string_view>

#include "kk/core/error.hpp"

namespace kk::core {

namespace fs = std::filesystem;

fs::path normalize(const fs::path& p);  // weakly_canonical, lexically normal
// True if `p` is `root` or inside it (both normalized; symlinks resolved).
bool is_within(const fs::path& root, const fs::path& p);
std::string rel_str(const fs::path& root, const fs::path& p);  // generic, '/' separators

Result<std::string> read_file(const fs::path& p);
// Atomic write: temp file in the same directory + rename. Creates parent dirs.
VoidResult write_file(const fs::path& p, std::string_view content);
VoidResult append_file(const fs::path& p, std::string_view content);

// "stream.ring" → "stream_ring" (CMake target names, 5.5).
std::string target_name(std::string_view block_id);

bool is_code_file(const fs::path& p);  // .cpp .cc .cxx .c .hpp .hh .h .hxx .ipp .inl

// Finds the project root: the nearest ancestor of `start` holding project.ksl.
Result<fs::path> find_project_root(const fs::path& start);

}  // namespace kk::core
