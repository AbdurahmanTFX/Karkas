// karkas init <dir> and karkas gen cmake|mocks|probe|skel (8.3, 13.1).
#include <fmt/format.h>

#include <iostream>

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
    std::string project = fmt::format("@project {}\n", name);
    if (!idea.empty()) project += core::format_line("idea", core::format_free_text(idea)) + "\n";
    project += "lang    cpp\nbuild   cmake-ninja\nstd     c++20\ntest    doctest\n"
               "rule    blocks talk only through api/ headers\n"
               "rule    no exceptions across block boundaries, use Result<T>\n";
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
    std::error_code ec;
    fs::create_directories(root / "blocks", ec);
    fs::create_directories(root / "tests" / "integration", ec);
    if (auto r = index::ensure_karkas_dir(root); !r) die(r.error().str());
    setup_logging(app, root, false);
    std::cout << "initialized " << name << " in " << root.string() << "\nnext: karkas design \"<idea>\"\n";
  });
});

}  // namespace
}  // namespace kk::cli
