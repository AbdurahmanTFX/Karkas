#include "kk/agents/run.hpp"

#include <fmt/format.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

#include "kk/code/gen.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

namespace {

bool has_sources(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec)) continue;
    auto ext = it->path().extension().string();
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return true;
  }
  return false;
}

bool is_elf_program(const fs::path& p) {
  struct stat st {};
  if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & S_IXUSR) == 0) return false;
  std::ifstream in(p, std::ios::binary);
  std::array<char, 4> magic{};
  in.read(magic.data(), magic.size());
  return in.gcount() == 4 && magic == std::array<char, 4>{'\x7f', 'E', 'L', 'F'};
}

}  // namespace

std::string shell_quote(const std::string& word) {
  bool plain = !word.empty() && std::ranges::all_of(word, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || std::string_view("/._-+=:,@%").find(c) != std::string_view::npos;
  });
  if (plain) return word;
  return "'" + core::replace_all(word, "'", "'\\''") + "'";
}

RunTarget run_target(const core::Schema& schema, const core::Config& cfg, const std::vector<std::string>& args) {
  RunTarget t;
  const fs::path& root = schema.root;
  // The generated program: the entry block's executable, named after the project (5.5).
  const core::Block* entry = nullptr;
  for (const auto& b : schema.blocks) {
    if (b.kind == core::BlockKind::Entry) {
      entry = &b;
      break;
    }
  }
  std::string why_not;
  const bool own_build = code::has_own_cmake(root) || !cfg.build_command.empty();
  if (own_build) {
    why_not = "the project has its own build, so Karkas does not know which program it makes";
  } else if (entry == nullptr) {
    why_not = "the schema has no entry block (the block with main())";
  } else if (entry->external()) {
    why_not = fmt::format("the entry block {} is imported code built by the project itself", entry->id);
  } else if (!has_sources(root / entry->dir / schema.project.dirs.impl)) {
    why_not = fmt::format("the entry block {} has no code yet (karkas implement writes it)", entry->id);
  } else if (schema.project.name.empty()) {
    why_not = "project.ksl names no project";
  } else {
    t.generated = (fs::path(cfg.build_dir) / schema.project.name).generic_string();
  }
  if (!cfg.run_command.empty()) {
    t.command = true;
    t.shown = cfg.run_command;
    t.argv = {"/bin/sh", "-c", args.empty() ? cfg.run_command : cfg.run_command + " \"$@\"", "sh"};
    t.argv.insert(t.argv.end(), args.begin(), args.end());
    for (const auto& a : args) t.shown += " " + shell_quote(a);
    return t;
  }
  if (t.generated.empty()) {
    t.error = "nothing to run: " + why_not + "; name the program in karkas.toml as [project] run_command";
    return t;
  }
  t.argv = {(root / t.generated).string()};
  t.argv.insert(t.argv.end(), args.begin(), args.end());
  t.shown = t.generated;
  for (const auto& a : args) t.shown += " " + shell_quote(a);
  return t;
}

std::vector<std::string> program_candidates(const fs::path& root, const core::Config& cfg, std::size_t max) {
  struct Found {
    fs::file_time_type time;
    std::string rel;
  };
  std::vector<Found> found;
  auto dir = root / cfg.build_dir;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return {};
  constexpr int kMaxDepth = 4;
  for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    auto name = it->path().filename().string();
    std::error_code fec;
    if (it->is_directory(fec)) {
      // CMake's own programs (compiler checks), fetched dependencies, test trees.
      if (it.depth() >= kMaxDepth - 1 || name == "CMakeFiles" || name == "_deps" || name == "Testing" || name.starts_with('.')) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (name.ends_with("_tests") || name.ends_with("_integration") || core::contains(name, ".so") || name.ends_with(".o")) continue;
    if (!is_elf_program(it->path())) continue;
    found.push_back({.time = fs::last_write_time(it->path(), fec), .rel = core::rel_str(root, it->path())});
  }
  std::ranges::sort(found, [](const Found& a, const Found& b) { return a.time != b.time ? a.time > b.time : a.rel < b.rel; });
  std::vector<std::string> out;
  for (const auto& f : found) {
    if (out.size() >= max) break;
    out.push_back(f.rel);
  }
  return out;
}

std::string run_target_help(const RunTarget& target, const std::vector<std::string>& candidates) {
  std::string out = target.error + "\n";
  if (!candidates.empty()) {
    out += "programs in the build folder:\n";
    for (const auto& c : candidates) out += "  " + c + "\n";
  }
  out += fmt::format("karkas.toml:\n  [project]\n  run_command = \"{}\"\n", candidates.empty() ? "build/<program>" : candidates.front());
  return out;
}

}  // namespace kk::agents
