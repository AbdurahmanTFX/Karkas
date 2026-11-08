#pragma once
// Schema = tree of blocks + graph of links (3.1), loaded from project.ksl and
// blocks/**/block.ksl. Block id == folder path joined with '.'.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/model.hpp"

namespace kk::core {

// A problem found by loading or checking (4.6, 8.2): E/W codes, S (syntax), D (drift).
struct Issue {
  std::string code;   // "E03", "W01", "S04", "D02", ...
  char severity = 'E';  // 'E' error (blocks agents), 'W' warning
  std::string block;  // block id, "" for project-level
  std::string message;
  SourceLoc loc;
  // "blocks/x/block.ksl:3:9: E E03 stream.pump: message"
  [[nodiscard]] std::string str() const;
};
bool has_errors(const std::vector<Issue>& issues);
// Compact text of issues for a model: every error in full; warnings counted per code, with up to
// `examples` lines of each code (issues of the blocks in `focus` are always shown in full).
std::string issues_summary(const std::vector<Issue>& issues, std::size_t examples = 2,
                           const std::vector<std::string>& focus = {});

// A link between two blocks, resolved from the consumer's card (3.5).
//   data/event: src = supplier (out port), dst = consumer (in port)
//   call:       src = caller,  dst = callee (fn)
//   use:        src = user,    dst = shared block
//   state:      src = declaring block, dst = with= block
struct LinkInfo {
  std::string src_block, src_port, dst_block, dst_port;
  LinkKind kind = LinkKind::Data;
  std::string via, sync, rate;
  std::string declared_in;
  int line = 0;
  std::string thread_src, thread_dst;
  [[nodiscard]] bool crosses_threads() const;
};

struct Schema {
  std::filesystem::path root;
  Project project;
  std::vector<Block> blocks;  // sorted by id
  std::vector<Issue> issues;  // issues found while loading (S.., E01)

  [[nodiscard]] const Block* find(std::string_view id) const;
  Block* find_mut(std::string_view id);
  // Children of `id` ("" = top level), sorted by id.
  [[nodiscard]] std::vector<const Block*> children(std::string_view id) const;
  [[nodiscard]] std::vector<const Block*> top_level() const { return children(""); }
  // All leaf-like (leaf/entry/shared) descendants of id ("" = whole project).
  [[nodiscard]] std::vector<const Block*> leaves_under(std::string_view id) const;
  [[nodiscard]] std::vector<std::string> ancestors(std::string_view id) const;  // nearest first
  [[nodiscard]] static bool is_under(std::string_view ancestor, std::string_view id);
  [[nodiscard]] std::filesystem::path blocks_root() const;
  [[nodiscard]] std::filesystem::path block_dir(std::string_view id) const;
  [[nodiscard]] std::vector<LinkInfo> links() const;
  // Order of children for views: entry first, then by data-flow (topological order of
  // leaves, ties by id) so views read left to right like the data (4.7).
  [[nodiscard]] std::vector<const Block*> ordered_children(std::string_view id) const;
  [[nodiscard]] std::vector<std::string> leaf_flow_order() const;
};

// Loads the schema. Fails only when project.ksl is missing or unreadable; card
// syntax errors become issues and the broken card is skipped.
Result<Schema> load_schema(const std::filesystem::path& root);

// Parses one card file into a block (for tools that edit cards).
Result<Block> load_block_file(const std::filesystem::path& file, std::string_view display);

// Child id → path parts: "stream.ring" → {"stream", "ring"}.
std::filesystem::path id_to_rel_path(std::string_view id);
std::string parent_id(std::string_view id);
std::string last_segment(std::string_view id);
int id_depth(std::string_view id);

}  // namespace kk::core
