#pragma once
// "Build and run" (docs/adr/0037): which program of the target project runs and how — without a
// model. `karkas run` and the green triangle of karkas-gui use it; the build itself is
// TestService::build_program (testing.hpp).
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "kk/core/config.hpp"
#include "kk/core/schema.hpp"

namespace kk::agents {

struct RunTarget {
  // The program and its arguments, run from the project root: {"/bin/sh", "-c", run_command, ...} for
  // [project] run_command, {<root>/<build_dir>/<project name>, args...} for the program of the entry block.
  std::vector<std::string> argv;
  std::string shown;     // what the human reads: the command, or the program's path from the project root
  bool command = false;  // [project] run_command
  // The program Karkas's generated CMake builds (from the project root), "" when the project has none
  // (its own build, an imported entry, no code yet): what run_command replaces.
  std::string generated;
  std::string error;  // nothing to run: why, and how to name the program
  [[nodiscard]] bool ok() const { return error.empty(); }
};

// [project] run_command when it is set (by sh -c; `args` follow it as "$@"); else the program of the entry
// block that Karkas's generated CMake builds. A project with its own build names its program in run_command.
RunTarget run_target(const core::Schema& schema, const core::Config& cfg, const std::vector<std::string>& args = {});

// Programs in the build folder, newest first, as paths from the project root: executable ELF files, without
// libraries, CMake's own programs and the test programs Karkas generates. What run_command may name.
std::vector<std::string> program_candidates(const std::filesystem::path& root, const core::Config& cfg, std::size_t max = 8);

// What `karkas run` says when nothing can be run: the reason, the programs found, the line for karkas.toml.
std::string run_target_help(const RunTarget& target, const std::vector<std::string>& candidates);

// One word for a shell: kept when it is plain, else in single quotes.
std::string shell_quote(const std::string& word);

}  // namespace kk::agents
