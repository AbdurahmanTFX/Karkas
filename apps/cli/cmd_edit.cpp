// karkas edit — deterministic schema edits from the command line (the operations of the GUI,
// stage 9): add-group, move, rename, purpose, remove, batch. Each edit is checked and applied at once;
// --dry-run shows the card diff only (ADR 0015).
#include <fmt/format.h>

#include <iostream>
#include <iterator>

#include "app.hpp"
#include "kk/agents/edit.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::cli {
namespace {

void run_edit(const fs::path& root, const kk::Result<agents::EditOp>& op, bool dry_run) {
  if (!op) die(op.error().message);
  if (dry_run) {
    std::cout << agents::preview_edit(root, *op);
    return;
  }
  auto a = agents::apply_edit(root, *op);
  if (!a) die(a.error().message);
  std::cout << op->title << " (" << a->files.size() << " cards written)\n";
}

core::Schema load(const fs::path& root) {
  auto s = core::load_schema(root);
  if (!s) die(s.error().str());
  return *s;
}

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("edit", "schema edits: add-group | move | rename | purpose | remove (checked, applied at once)");
  cmd->require_subcommand(1);
  static bool dry_run = false;
  static std::string id, target, text;
  cmd->add_flag("--dry-run", dry_run, "show the card diff without writing");
  auto root = [&app] {
    auto r = app.root();
    setup_logging(app, r, false);
    return r;
  };

  auto* add = cmd->add_subcommand("add-group", "new group <parent>.<name> (or <name> at the top level)");
  add->add_option("id", id, "id of the new group")->required();
  add->add_option("--purpose", text, "one English sentence")->required();
  add->callback([root] {
    auto r = root();
    auto pos = id.rfind('.');
    std::string parent = pos == std::string::npos ? "root" : id.substr(0, pos);
    std::string name = pos == std::string::npos ? id : id.substr(pos + 1);
    run_edit(r, agents::op_add_block(load(r), parent, name, core::BlockKind::Group, text, ""), dry_run);
  });

  auto* move = cmd->add_subcommand("move", "move a block (and its subtree) under a group; root = top level");
  move->add_option("id", id, "block to move")->required();
  move->add_option("parent", target, "group id or root")->required();
  move->callback([root] {
    auto r = root();
    run_edit(r, agents::op_move(load(r), id, target), dry_run);
  });

  auto* rename = cmd->add_subcommand("rename", "new name of a block in the same group");
  rename->add_option("id", id, "block")->required();
  rename->add_option("name", target, "new last segment of the id")->required();
  rename->callback([root] {
    auto r = root();
    run_edit(r, agents::op_rename(load(r), id, target), dry_run);
  });

  auto* purpose = cmd->add_subcommand("purpose", "replace the purpose of a block");
  purpose->add_option("id", id, "block")->required();
  purpose->add_option("text", text, "one English sentence")->required();
  purpose->callback([root] {
    auto r = root();
    run_edit(r, agents::op_set_purpose(load(r), id, text), dry_run);
  });

  static std::string script;
  auto* batch = cmd->add_subcommand("batch", "apply a script of edits in one go (one per line; - = stdin)");
  batch->add_option("script", script, "file with lines: add-group <id> \"purpose\" | move <id> <group|root> | "
                                      "rename <id> <name> | purpose <id> \"text\" | remove <id>")->required();
  batch->callback([root] {
    auto r = root();
    std::string script_text;
    if (script == "-") {
      script_text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    } else {
      auto t = core::read_file(script);
      if (!t) die(t.error().message);
      script_text = *t;
    }
    auto res = agents::apply_edit_script(r, script_text);
    std::cout << fmt::format("applied {} edits\n", res.applied);
    if (!res.error.empty()) die(res.error);
  });

  auto* remove = cmd->add_subcommand("remove", "remove a block without children (imported files stay on disk)");
  remove->add_option("id", id, "block")->required();
  remove->callback([root] {
    auto r = root();
    run_edit(r, agents::op_remove_block(load(r), id), dry_run);
  });
});

}  // namespace
}  // namespace kk::cli
