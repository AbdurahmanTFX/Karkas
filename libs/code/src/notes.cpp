#include "kk/code/notes.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "kk/core/model.hpp"
#include "kk/core/notes.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"

namespace kk::code {

namespace fs = std::filesystem;

namespace {

bool is_note_file(const fs::path& p) { return core::to_lower(p.extension().string()) == ".txt"; }

// Folders that hold no notes of the user: hidden ones (.git, .karkas), build trees, Karkas's cards.
bool skipped_dir(const std::string& name, const std::string& blocks) {
  return name.starts_with(".") || name.starts_with("build") || name == blocks || name == "third_party" || name == "_deps" ||
         name == "node_modules";
}

template <typename F>
void each_note_file(const fs::path& root, const std::string& blocks, F f) {
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code fec;
    if (it->is_directory(fec)) {
      if (skipped_dir(it->path().filename().string(), blocks)) it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(fec) && is_note_file(it->path()) && !f(it->path())) return;
  }
}

}  // namespace

bool has_notes(const fs::path& dir) {
  bool found = false;
  each_note_file(dir, "blocks", [&](const fs::path&) {
    found = true;
    return false;
  });
  return found;
}

kk::Result<NotesImport> import_notes(const fs::path& root_in) {
  const auto root = core::normalize(root_in);
  auto schema = core::load_schema(root);
  if (!schema) return std::unexpected(schema.error());
  std::set<std::string> claimed;
  std::map<std::string, const core::Block*> blocks;
  for (const auto& b : schema->blocks) {
    blocks[b.id] = &b;
    for (const auto& f : b.file_list()) claimed.insert(f);
  }
  std::vector<std::string> files;
  each_note_file(root, schema->project.dirs.blocks, [&](const fs::path& p) {
    auto rel = core::rel_str(root, p);
    if (!claimed.contains(rel)) files.push_back(rel);
    return true;
  });
  std::sort(files.begin(), files.end());
  NotesImport out;
  std::set<std::string> taken;
  for (const auto& [id, b] : blocks) taken.insert(id);
  auto unique_child = [&](const std::string& parent, const std::string& seg) {
    auto id = parent.empty() ? seg : parent + "." + seg;
    for (int n = 2; taken.contains(id); ++n) id = (parent.empty() ? seg : parent + "." + seg) + "_" + std::to_string(n);
    taken.insert(id);
    return id;
  };
  auto write_card = [&](const std::string& id, const std::string& title, bool group, const std::string& file) -> kk::VoidResult {
    core::Block b;
    b.id = id;
    b.purpose.value = title;
    b.purpose.set = true;
    b.kind_text.value = group ? "group" : "leaf";
    b.kind_text.set = true;
    b.kind = group ? core::BlockKind::Group : core::BlockKind::Leaf;
    if (!file.empty()) b.files.push_back(core::FilesDecl{{file}, {}, 0, 0});
    return core::write_file(schema->blocks_root() / core::id_to_rel_path(id) / "block.ksl", core::write_block(b));
  };
  std::map<std::string, std::string> topic_of;  // folder (relative) → its topic
  std::function<kk::Result<std::string>(const fs::path&)> topic = [&](const fs::path& dir) -> kk::Result<std::string> {
    auto key = dir.generic_string();
    if (key.empty() || key == ".") return std::string();
    if (auto it = topic_of.find(key); it != topic_of.end()) return it->second;
    auto parent = topic(dir.parent_path());
    if (!parent) return parent;
    auto name = dir.filename().string();
    auto seg = core::ident_from_title(name);
    auto same = parent->empty() ? seg : *parent + "." + seg;
    std::string id;
    if (auto b = blocks.find(same); b != blocks.end() && b->second->kind == core::BlockKind::Group) {
      id = same;  // the topic is there already (an import run before)
    } else {
      id = unique_child(*parent, seg);
      if (auto r = write_card(id, name, true, ""); !r) return std::unexpected(r.error());
      out.topics.push_back(id);
    }
    topic_of[key] = id;
    return id;
  };
  for (const auto& rel : files) {
    fs::path p(rel);
    auto parent = topic(p.parent_path());
    if (!parent) return std::unexpected(parent.error());
    auto title = p.stem().string();
    auto id = unique_child(*parent, core::ident_from_title(title));
    if (auto r = write_card(id, title, false, rel); !r) return std::unexpected(r.error());
    out.notes.push_back(id);
  }
  return out;
}

}  // namespace kk::code
