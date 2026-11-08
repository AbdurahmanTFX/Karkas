// karkas init <dir> and karkas gen cmake|mocks|probe|skel (8.3, 13.1).
#include <fmt/format.h>

#include <iostream>
#include <regex>

#include "app.hpp"
#include "kk/code/gen.hpp"
#include "kk/code/init.hpp"
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
  auto* cmd = cli.add_subcommand("gen", "deterministic generation: cmake | mocks | probe | skel | guide");
  static std::string what;
  static std::string block = "all";
  cmd->add_option("what", what, "cmake | mocks | probe | skel | all | guide (Karkas block of CLAUDE.md)")
      ->required()
      ->check(CLI::IsMember({"cmake", "mocks", "probe", "skel", "all", "guide"}));
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
    if (what == "guide") {
      auto g = code::write_agent_guide(root);
      if (!g) die(g.error().str());
      std::cout << "CLAUDE.md: " << *g << "\n";
    }
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
    auto r = code::init_project(root, idea);
    if (!r) die(r.error().message);
    for (const auto& f : r->written) std::cout << "wrote " << f << "\n";
    setup_logging(app, root, false);
    std::cout << "initialized " << r->name << " in " << root.string() << "\n";
    if (r->existing_code) {
      std::cout << "next: karkas import   (existing code becomes blocks; files stay where they are)\n";
    } else {
      std::cout << "next: karkas design \"<idea>\"\n";
    }
  });
});

}  // namespace
}  // namespace kk::cli
