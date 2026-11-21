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

// A build tree by its folder name: build, build-debug, build_linux, build.x, build64, buildRelease — not
// builder or buildings ("build" then a lowercase letter is a word of its own: leveldb's db/builder).
bool is_build_dir_name(std::string_view name);
// A folder to pass over as a build tree: such a name and no block.ksl in it (a block may be called
// build_steps). Does not check that `dir` is a folder.
bool is_build_dir(const fs::path& dir);

// The running executable. After a rebuild replaced it, /proc/self/exe reads "<path> (deleted)":
// the new file at <path> is taken then (bugs.txt 9). "" when it cannot be found.
fs::path self_executable();

// Finds the project root: the nearest ancestor of `start` holding project.ksl.
Result<fs::path> find_project_root(const fs::path& start);

}  // namespace kk::core
