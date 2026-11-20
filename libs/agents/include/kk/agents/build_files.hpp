#pragma once
// The build files of an imported project (docs/adr/0030). Its CMakeLists.txt belongs to no block, so no worker
// may edit it, and a new source file of a block never reached the build: Endless Sky's round 3 ended with a
// link error ("undefined reference to PilotStatisticsPanel::PilotStatisticsPanel") after every block was done.
// Deterministic first: Karkas itself adds a block's file that no CMakeLists.txt names yet, next to a file of
// the same folder that one names, in the list's order and format (one file per line, or files on one line).
#include <filesystem>
#include <string>
#include <vector>

namespace kk::agents {

// Adds the code files among `files` (paths relative to `root`) that exist and that no CMakeLists.txt of the
// project names, to the CMakeLists.txt that names another code file of their folder; a header only where the
// list names headers of that folder too. Returns one line per changed build file:
// "source/CMakeLists.txt: PilotStatisticsPanel.cpp, PilotStatisticsPanel.h". A project without such a list (a
// glob, another build system) is left alone.
std::vector<std::string> add_sources_to_build(const std::filesystem::path& root, const std::vector<std::string>& files);

// The names a linker error line points at, most specific first: "undefined reference to
// `A::B::f(int)'" -> {"A", "B", "f"}; also "multiple definition of", lld's "undefined symbol:", "vtable for X".
// Empty when the line is not such an error.
std::vector<std::string> link_error_names(const std::string& line);

}  // namespace kk::agents
