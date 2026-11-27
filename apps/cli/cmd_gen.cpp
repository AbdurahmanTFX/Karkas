// karkas init <dir> and karkas gen cmake|mocks|probe|skel (8.3, 13.1).
#include <fmt/format.h>

#include <algorithm>
#include <iostream>
#include <regex>

#include "app.hpp"
#include "kk/code/gen.hpp"
#include "kk/code/init.hpp"
#include "kk/core/config.hpp"
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
  for (const auto& f : r->removed) std::cout << "removed " << f << "\n";
  if (r->written.empty() && r->skipped.empty() && r->removed.empty()) std::cout << "up to date\n";
}

AutoRegister reg_gen([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("gen", "deterministic generation: cmake | mocks | probe | skel | guide | agents");
  static std::string what;
  static std::string block = "all";
  cmd->add_option("what", what, "cmake | mocks | probe | skel | all | guide (Karkas block of CLAUDE.md) | agents (Claude Code subagents of the model tiers)")
      ->required()
      ->check(CLI::IsMember({"cmake", "mocks", "probe", "skel", "all", "guide", "agents"}));
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
    if (what == "agents") {
      auto cfg = core::load_config(root);
      if (!cfg) die(cfg.error().str());
      report(code::write_claude_agents(root, *cfg));
    }
  });
});

AutoRegister reg_init([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("init", "create a target project");
  static std::string dir;
  static std::string idea;
  cmd->add_option("dir", dir, "project directory (its name becomes the project id)")->required();
  cmd->add_option("--idea", idea, "one-line idea (can be set later by karkas design)");
  static bool notes = false;
  static bool code_project = false;
  cmd->add_flag("--notes", notes, "a notes project: text notes linked by [[name]] (default: when the folder holds .txt notes and no code)");
  cmd->add_flag("--code", code_project, "a code project even when the folder holds only .txt notes");
  cmd->callback([&app] {
    fs::path root = core::normalize(dir);
    auto kind = notes ? code::ProjectKind::Notes : code_project ? code::ProjectKind::Code : code::ProjectKind::Auto;
    auto r = code::init_project(root, idea, kind);
    if (!r) die(r.error().message);
    for (const auto& f : r->written) std::cout << "wrote " << f << "\n";
    setup_logging(app, root, false);
    std::cout << "initialized " << r->name << (r->notes ? " (notes)" : "") << " in " << root.string() << "\n";
    if (r->notes) {
      if (!r->imported.empty()) std::cout << "cards for the notes already there: " << r->imported.size() << " (the files stay in place)\n";
      std::cout << "next: karkas gui   (write notes; [[name]] links one note to another)\n";
    } else if (r->existing_code) {
      if (!r->langs.empty()) std::cout << "languages: " << core::join(r->langs, " ") << " (lang of project.ksl)\n";
      std::cout << "next: karkas import   (existing code becomes blocks; files stay where they are)\n";
      if (std::find(r->langs.begin(), r->langs.end(), "cpp") == r->langs.end()) {
        std::cout << "then: set [project] build_command and [test] command in karkas.toml (Karkas generates builds for C++ only)\n";
      }
    } else {
      std::cout << "next: karkas design \"<idea>\"\n";
    }
  });
});

}  // namespace
}  // namespace kk::cli
