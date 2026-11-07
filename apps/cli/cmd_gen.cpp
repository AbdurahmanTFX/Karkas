// karkas init <dir> and karkas gen cmake|mocks|probe|skel (8.3, 13.1).
#include <fmt/format.h>

#include <iostream>
#include <regex>

#include "app.hpp"
#include "kk/code/gen.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/index/state.hpp"

namespace kk::cli {
namespace {

void report(const kk::Result<code::GenResult>& r) {
  if (!r) die(r.error().str());
  for (const auto& f : r->written) std::cout << "wrote " << f << "\n";
  for (const auto& f : r->skipped) std::cout << "kept  " << f << " (hand-written)\n";
  if (r->written.empty() && r->skipped.empty()) std::cout << "up to date\n";
}

AutoRegister reg_gen([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("gen", "deterministic generation: cmake | mocks | probe | skel");
  static std::string what;
  static std::string block = "all";
  cmd->add_option("what", what, "cmake | mocks | probe | skel | all")
      ->required()
      ->check(CLI::IsMember({"cmake", "mocks", "probe", "skel", "all"}));
  cmd->add_option("block", block, "block for mocks/skel (default: all)");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto load = [&] {
      auto s = core::load_schema(root);
      if (!s) die(s.error().str());
      return *s;
    };
    if (what == "probe" || what == "all") report(code::write_probe(load()));
    if (what == "skel" || what == "all") report(code::write_skel(load(), block));
    if (what == "mocks" || what == "all") report(code::write_mocks(load(), block));
    if (what == "cmake" || what == "all") report(code::write_cmake(load()));
  });
});

AutoRegister reg_init([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("init", "create a target project");
  static std::string dir;
  static std::string idea;
  cmd->add_option("dir", dir, "project directory (its name becomes the project id)")->required();
  cmd->add_option("--idea", idea, "one-line idea (can be set later by karkas design)");
  cmd->callback([&app] {
    fs::path root = core::normalize(dir);
    auto name = core::to_lower(root.filename().string());
    for (auto& c : name) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
    }
    if (name.empty() || !(name[0] >= 'a' && name[0] <= 'z')) die("project folder name must start with a letter");
    if (fs::exists(root / "project.ksl")) die(root.string() + " already holds a project.ksl");
    // An existing project: keep its C++ standard and do not invent rules for its code.
    bool existing = false;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && !existing; it.increment(ec)) {
      if (ec) break;
      auto n = it->path().filename().string();
      if (it->is_directory(ec) && (n.starts_with(".") || n.starts_with("build") || n == "third_party" || n == "_deps")) {
        it.disable_recursion_pending();
        continue;
      }
      existing = it->is_regular_file(ec) && core::is_code_file(it->path());
    }
    std::string cxx = "20";
    if (auto cm = core::read_file(root / "CMakeLists.txt")) {
      std::smatch m;
      if (std::regex_search(*cm, m, std::regex(R"(CMAKE_CXX_STANDARD\s+(\d+))")) ||
          std::regex_search(*cm, m, std::regex(R"(cxx_std_(\d+))"))) {
        cxx = m[1].str();
      }
    }
    std::string project = fmt::format("@project {}\n", name);
    if (!idea.empty()) project += core::format_line("idea", core::format_free_text(idea)) + "\n";
    project += fmt::format("lang    cpp\nbuild   cmake-ninja\nstd     c++{}\ntest    doctest\n", cxx);
    if (!existing) {
      project += "rule    blocks talk only through api/ headers\n"
                 "rule    no exceptions across block boundaries, use Result<T>\n";
    }
    auto w = [&](const fs::path& p, std::string_view text) {
      if (fs::exists(p)) return;
      if (auto r = core::write_file(p, text); !r) die(r.error().str());
      std::cout << "wrote " << core::rel_str(root, p) << "\n";
    };
    w(root / "project.ksl", project);
    auto toml = code::target_template("karkas.toml");
    if (!toml) die(toml.error().message);
    w(root / "karkas.toml", *toml);
    auto user = code::target_template("user.cmake");
    w(root / "cmake" / "user.cmake", user.value_or(""));
    auto gi = code::target_template("gitignore");
    w(root / ".gitignore", gi.value_or(""));
    fs::create_directories(root / "blocks", ec);
    fs::create_directories(root / "tests" / "integration", ec);
    if (auto r = index::ensure_karkas_dir(root); !r) die(r.error().str());
    setup_logging(app, root, false);
    std::cout << "initialized " << name << " in " << root.string() << "\n";
    if (existing) {
      std::cout << "next: karkas import   (existing code becomes blocks; files stay where they are)\n";
    } else {
      std::cout << "next: karkas design \"<idea>\"\n";
    }
  });
});

}  // namespace
}  // namespace kk::cli
