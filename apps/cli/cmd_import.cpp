// karkas import — existing C/C++ code becomes blocks without moving files (stage 11,
// docs/adr/0009). By default the new cards go to a @change for approval.
#include <fmt/format.h>

#include <iostream>

#include "app.hpp"
#include "kk/agents/changes.hpp"
#include "kk/code/import.hpp"
#include "kk/core/text.hpp"

namespace kk::cli {
namespace {

AutoRegister reg_import([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("import", "cut existing C/C++ code into blocks (files stay in place)");
  static bool apply = false;
  static bool dry_run = false;
  static std::vector<std::string> exclude;
  cmd->add_flag("--apply", apply, "write the cards at once instead of proposing a change");
  cmd->add_flag("--dry-run", dry_run, "only show what would be created");
  cmd->add_option("--exclude", exclude, "folder to skip (project-relative), can be repeated");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    code::ImportOptions opts;
    opts.exclude = exclude;
    auto plan = code::plan_import(root, opts);
    if (!plan) die(plan.error().str());
    if (plan->blocks.empty()) {
      std::cout << "nothing to import: every code file already belongs to a block\n";
      return;
    }
    std::cout << code::describe_plan(*plan);
    if (dry_run) return;
    if (apply) {
      auto r = agents::apply_diff(root, plan->diff);
      if (!r) die(r.error().str());
      if (!r->issues.empty()) {
        std::string text;
        for (const auto& i : r->issues) text += "  " + i.str() + "\n";
        die("the imported schema does not pass the checks:\n" + text);
      }
      std::cout << fmt::format("\nwrote {} cards; see them with karkas gui or karkas view\n", r->files.size());
      return;
    }
    agents::ChangeStore store(root);
    auto c = store.create("import", fmt::format("karkas import: {} blocks over {} existing files", plan->blocks.size(), plan->files.size()),
                          plan->diff);
    if (!c) die(c.error().str());
    std::cout << fmt::format("\nproposed {}: review it in karkas gui (tab Changes) or with karkas change show {}\n"
                             "approve: karkas approve {}   reject: karkas change reject {}\n",
                             c->id, c->id, c->id, c->id);
  });
});

}  // namespace
}  // namespace kk::cli
