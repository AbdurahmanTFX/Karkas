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

kk::VoidResult add_note_link(const core::Schema& s, const std::string& src, const std::string& dst) {
  const auto* a = s.find(src);
  if (a == nullptr || s.find(dst) == nullptr) return kk::fail("notes", "no note " + (a == nullptr ? src : dst));
  auto file = core::note_file(*a);
  if (file.empty()) return kk::fail("notes", src + " is a topic: a link starts in a note");
  auto text = core::read_file(s.root / file).value_or("");
  if (!text.empty() && !text.ends_with('\n')) text += '\n';
  text += "[[" + core::note_link_name(s, dst) + "]]\n";
  return core::write_file(s.root / file, text);
}

kk::Result<int> remove_note_link(const core::Schema& s, const std::string& src, const std::string& dst) {
  const auto* a = s.find(src);
  if (a == nullptr) return kk::fail("notes", "no note " + src);
  auto file = core::note_file(*a);
  if (file.empty()) return 0;
  auto text = core::read_file(s.root / file);
  if (!text) return 0;
  int n = 0;
  auto out = core::rewrite_note_refs(*text, [&](const std::string& name, const std::string& inner) -> std::optional<std::string> {
    auto fits = core::resolve_note(s, name);
    if (fits.size() != 1 || fits.front() != dst) return std::nullopt;
    ++n;
    auto bar = inner.find('|');
    return std::string(core::trim(bar == std::string::npos ? std::string_view(inner) : std::string_view(inner).substr(bar + 1)));
  });
  if (n > 0) {
    if (auto r = core::write_file(s.root / file, out); !r) return std::unexpected(r.error());
  }
  return n;
}

kk::Result<std::vector<std::string>> retitle_note(const core::Schema& s, const std::string& id, const std::string& title) {
  const auto* b = s.find(id);
  if (b == nullptr) return kk::fail("notes", "no note " + id);
  auto t = std::string(core::trim(title));
  if (t.empty()) return kk::fail("notes", "a title cannot be empty");
  auto card_path = s.root / b->dir / "block.ksl";
  auto card_text = core::read_file(card_path);
  if (!card_text) return std::unexpected(card_text.error());
  auto card = core::parse_block_text(*card_text, b->dir + "/block.ksl");
  if (!card) return std::unexpected(card.error());
  const auto old_title = core::fold_title(b->purpose.value);
  card->purpose.value = t;
  card->purpose.set = true;
  if (auto r = core::write_file(card_path, core::write_block(*card)); !r) return std::unexpected(r.error());
  // Links that named it by its old title follow; links by id stay as they are.
  std::vector<std::string> changed;
  for (const auto& x : s.blocks) {
    auto file = core::note_file(x);
    if (file.empty()) continue;
    bool names_it = false;
    for (const auto& r : x.refs) names_it = names_it || (r.target == id && core::fold_title(r.name) == old_title);
    if (!names_it) continue;
    auto text = core::read_file(s.root / file);
    if (!text) continue;
    auto out = core::rewrite_note_refs(*text, [&](const std::string& name, const std::string& inner) -> std::optional<std::string> {
      if (core::fold_title(name) != old_title) return std::nullopt;
      auto fits = core::resolve_note(s, name);
      if (fits.size() != 1 || fits.front() != id) return std::nullopt;
      auto bar = inner.find('|');
      return "[[" + t + (bar == std::string::npos ? std::string() : inner.substr(bar)) + "]]";
    });
    if (out == *text) continue;
    if (auto r = core::write_file(s.root / file, out); !r) return std::unexpected(r.error());
    changed.push_back(x.id);
  }
  return changed;
}

}  // namespace kk::code
