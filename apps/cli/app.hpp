#pragma once
// Shared pieces of the karkas CLI: context, output helpers, command registration.
#include <CLI/CLI.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kk::cli {

namespace fs = std::filesystem;

struct App {
  std::string project_opt;  // --project
  bool verbose = false;
  // Resolves the project root (from --project or by searching upwards).
  [[nodiscard]] fs::path root() const;
};

// Each command file provides a register function; main() calls them all.
using Register = std::function<void(CLI::App&, App&)>;
std::vector<Register>& registry();
struct AutoRegister {
  explicit AutoRegister(Register r) { registry().push_back(std::move(r)); }
};

// Exit with a message on stderr.
[[noreturn]] void die(const std::string& message, int code = 1);
void setup_logging(const App& app, const fs::path& project_root, bool mcp_mode);

}  // namespace kk::cli

#include <memory>

namespace kk::index {
class Index;
class ICodeAnalyzer;
}  // namespace kk::index

namespace kk::cli {
// The code analyzer used by this build (kk_code), or nullptr.
index::ICodeAnalyzer* analyzer();
// Opens the project index and brings it up to date (incremental).
std::unique_ptr<index::Index> open_index(const fs::path& root, bool update = true, bool full = false);
}  // namespace kk::cli
