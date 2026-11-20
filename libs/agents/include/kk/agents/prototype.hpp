#pragma once
// The prototype (docs/adr/0035): blocks and dependencies the human sketches on the schema in
// karkas-gui, kept in .karkas/prototype.json. The architect develops the schema from the idea and
// this sketch; the program checks — without a model — that the sketch's blocks and dependencies
// are kept (or each deviation is explained), and the architect's better variants of critical
// places become changes C-NNNN the human approves or rejects.
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kk/core/error.hpp"
#include "kk/core/schema.hpp"

namespace kk::agents {

struct SketchBlock {
  std::string name;     // identifier; the block id is <scope>.<name> (or <name> at the top level)
  std::string purpose;  // optional, the human's words
  std::string kind;     // "" (the architect decides) | group | leaf | entry | shared
  float x = 0, y = 0;   // place on the canvas
};

// A dependency the human drew: `from` uses / sends to / calls `to` (block ids: blocks of the
// sketch or blocks that exist).
struct SketchLink {
  std::string from, to;
  std::string note;  // optional: what goes along it
};

struct Sketch {
  std::string scope;  // the level it is drawn on: "" = the top level, else a group id
  std::string idea;   // the request in the human's words
  std::vector<SketchBlock> blocks;
  std::vector<SketchLink> links;
  std::map<std::string, std::string> map;  // sketch block id → the block that realizes it (renamed by the architect)
  std::string sent;   // "" a draft; "design" | "expand" | "task": given to the agents
  std::int64_t sent_at = 0;

  [[nodiscard]] std::string id_of(const SketchBlock& b) const { return scope.empty() ? b.name : scope + "." + b.name; }
  [[nodiscard]] const SketchBlock* find(std::string_view id) const;
  [[nodiscard]] bool empty() const { return blocks.empty() && links.empty(); }
};

std::filesystem::path sketch_path(const std::filesystem::path& root);  // .karkas/prototype.json
kk::Result<Sketch> load_sketch(const std::filesystem::path& root);      // an empty sketch when there is none
kk::VoidResult save_sketch(const std::filesystem::path& root, const Sketch& s);
kk::VoidResult clear_sketch(const std::filesystem::path& root);

// What is wrong with the sketch itself: names (identifiers, unique, not an existing block of
// its level unless `allow_existing`), kinds, link ends (a block of the sketch or one that
// exists), self links, repeated links. "" when it can be given to the agents.
std::string sketch_problems(const Sketch& s, const std::set<std::string>& existing_blocks);

// The sketch as an agent reads it.
std::string sketch_text(const Sketch& s);

// The task for the orchestrator when the level already has blocks: add what the sketch shows.
std::string sketch_task(const Sketch& s);

// What the check needs of a schema (core::Schema or the index snapshot of karkas-gui).
struct SchemaShape {
  std::set<std::string> blocks;
  std::set<std::string> childless_groups;  // groups not expanded yet: links through them come later
  std::vector<std::pair<std::string, std::string>> links;  // block → block, any kind
};
SchemaShape shape_of(const core::Schema& s);

struct SketchCheck {
  std::map<std::string, std::string> found;  // sketch block id → the block that realizes it ("" = missing)
  std::vector<int> links;                    // per sketch link: 1 kept, 0 missing, -1 pending (an end not expanded yet)
  std::vector<std::string> gaps;             // "block a is not in the schema", "dependency a -> b is not kept"
  [[nodiscard]] std::size_t kept_blocks() const;
  [[nodiscard]] std::size_t kept_links() const;
};
// A sketch block is kept by the block of the same id or the one `map` names; a dependency
// a -> b by a link (either direction) between a block of a's subtree and one of b's.
SketchCheck check_sketch(const Sketch& s, const SchemaShape& shape);

// The gaps that `deviations` does not explain: a block's gap is explained when the text names
// the block, a dependency's when it names both ends.
std::vector<std::string> unexplained_gaps(const Sketch& s, const SketchCheck& c, std::string_view deviations);

// "a -> b" lines (also "a: b", "a = b"), one per line or separated by ';' or ','.
std::map<std::string, std::string> parse_sketch_map(std::string_view text);

}  // namespace kk::agents
