#pragma once
// Schema edits made by a human on the picture (14.6, stage 9): each edit is shown as a
// KSL diff, applied after confirmation, checked (karkas check) and can be undone.
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kk/core/model.hpp"
#include "kk/core/schema.hpp"

namespace kk::agents {

struct EditOp {
  std::string title;                  // "add leaf dsp.reverb"
  std::vector<core::DiffLine> diff;   // card changes (10.7 format)
  // Rename only: folder move (relative to the project root) and id prefix change.
  std::string rename_from, rename_to;
  std::vector<std::string> tolerate;  // E codes accepted for this edit (e.g. E06 for a new empty group)
};

kk::Result<EditOp> op_add_block(const core::Schema& s, const std::string& parent, const std::string& name,
                                core::BlockKind kind, const std::string& purpose, const std::string& thread = "main");
kk::Result<EditOp> op_remove_block(const core::Schema& s, const std::string& id);
// dir: in | out | fn; spec: "<Type>" for in/out, "<name>(<params>) -> <R>" for fn (name taken from spec).
kk::Result<EditOp> op_add_port(const core::Schema& s, const std::string& id, core::PortDir dir, const std::string& name,
                               const std::string& spec);
kk::Result<EditOp> op_remove_port(const core::Schema& s, const std::string& id, const std::string& port);
// Out port → consumer (existing in port without from=, or a new in port named like the out
// port); fn → "call" line in the consumer card.
kk::Result<EditOp> op_connect(const core::Schema& s, const std::string& from_block, const std::string& from_port,
                              const std::string& to_block, const std::string& to_port = "");
// Removes one link, given as in the index: data/event (consumer loses from=), call/use/state
// (the line of the declaring card goes away).
kk::Result<EditOp> op_disconnect(const core::Schema& s, const std::string& kind, const std::string& src_block,
                                 const std::string& src_port, const std::string& dst_block, const std::string& dst_port);
kk::Result<EditOp> op_rename(const core::Schema& s, const std::string& id, const std::string& new_name);
// Moves a block (with its subtree) under another group ("" or "root" = the top level): its id
// becomes <parent>.<name>, the card folder moves and every reference is rewritten. Imported
// code stays where it is (files lines are project-relative). A group left empty is allowed (E06);
// remove it afterwards.
kk::Result<EditOp> op_move(const core::Schema& s, const std::string& id, const std::string& new_parent);
// Replaces the purpose line of a card.
kk::Result<EditOp> op_set_purpose(const core::Schema& s, const std::string& id, const std::string& purpose);
// The card of `id` as a human typed it in karkas-gui (docs/adr/0033): parsed, then turned into
// the lines removed and added (comments stay as they are; ports keep their order unless a line
// changes). Refused when it does not parse, names another block or changes nothing.
kk::Result<EditOp> op_replace_card(const core::Schema& s, const std::string& id, std::string_view text);
// Replaces the guarantees (inv lines, 3.8) of a leaf or shared block; empty texts are dropped.
kk::Result<EditOp> op_set_invariants(const core::Schema& s, const std::string& id, const std::vector<std::string>& invariants);
// Moves the ports, links and invariants of `absorb` into `keep` and removes `absorb`.
kk::Result<EditOp> op_merge(const core::Schema& s, const std::string& keep, const std::string& absorb);
// Moves `ports` of `id` into a new sibling block `new_name`.
kk::Result<EditOp> op_split(const core::Schema& s, const std::string& id, const std::string& new_name,
                            const std::vector<std::string>& ports);

struct AppliedEdit {
  std::string title;
  std::vector<std::pair<std::filesystem::path, std::optional<std::string>>> before;  // previous card texts
  std::string moved_from, moved_to;                                                 // folders (absolute)
  std::vector<std::string> files;
};

// A script of edits, one per line ("#" comments; quotes keep spaces together):
//   add-group <id> <purpose>   move <id> <group|root>   rename <id> <name>
//   purpose <id> <text>        remove <id>
// Applied in order, each checked like a single edit; stops at the first failing line.
struct EditScriptResult {
  int applied = 0;
  std::vector<std::string> titles;
  std::string error;  // "line 7: ..." when a line failed (the edits before it stay applied)
};
EditScriptResult apply_edit_script(const std::filesystem::path& root, std::string_view script);

// Preview text: the card diff (and the folder move of a rename).
std::string preview_edit(const std::filesystem::path& root, const EditOp& op);
// Applies the edit; nothing changes when the result breaks the schema checks.
kk::Result<AppliedEdit> apply_edit(const std::filesystem::path& root, const EditOp& op);
// Restores the cards (and folder) as they were before `e`.
kk::VoidResult undo_edit(const std::filesystem::path& root, const AppliedEdit& e);

}  // namespace kk::agents
