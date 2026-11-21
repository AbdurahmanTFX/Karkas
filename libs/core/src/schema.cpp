#include "kk/core/schema.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <queue>
#include <set>

#include "kk/core/markdown.hpp"
#include "kk/core/notes.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::core {

std::string Issue::str() const {
  std::string out;
  if (!loc.file.empty()) {
    out += loc.file;
    if (loc.line > 0) out += fmt::format(":{}", loc.line);
    if (loc.col > 0) out += fmt::format(":{}", loc.col);
    out += ": ";
  }
  out += fmt::format("{} {}", severity, code);
  if (!block.empty()) out += " " + block;
  out += ": " + message;
  return out;
}

bool has_errors(const std::vector<Issue>& issues) {
  return std::any_of(issues.begin(), issues.end(), [](const Issue& i) { return i.severity == 'E'; });
}

std::string issues_summary(const std::vector<Issue>& issues, std::size_t examples, const std::vector<std::string>& focus) {
  auto focused = [&](const Issue& i) {
    return std::any_of(focus.begin(), focus.end(), [&](const std::string& f) { return Schema::is_under(f, i.block); });
  };
  std::string out;
  std::map<std::string, std::vector<const Issue*>> warn;
  for (const auto& i : issues) {
    if (i.severity == 'E' || focused(i)) {
      out += i.str() + "\n";
    } else {
      warn[i.code].push_back(&i);
    }
  }
  if (!warn.empty()) {
    std::vector<std::string> counts;
    for (const auto& [code, list] : warn) counts.push_back(fmt::format("{} x{}", code, list.size()));
    out += "warnings by code: " + join(counts, ", ") + "\n";
    for (const auto& [code, list] : warn) {
      for (std::size_t k = 0; k < list.size() && k < examples; ++k) out += "  e.g. " + list[k]->str() + "\n";
    }
  }
  return out;
}

bool LinkInfo::crosses_threads() const {
  return !thread_src.empty() && !thread_dst.empty() && thread_src != thread_dst &&
         thread_src != "any" && thread_dst != "any";
}

// --- id helpers ---------------------------------------------------------------------------

std::filesystem::path id_to_rel_path(std::string_view id) {
  std::filesystem::path p;
  for (const auto& part : split(id, '.')) p /= part;
  return p;
}

std::string parent_id(std::string_view id) {
  auto dot = id.rfind('.');
  return dot == std::string_view::npos ? std::string() : std::string(id.substr(0, dot));
}

std::string last_segment(std::string_view id) {
  auto dot = id.rfind('.');
  return std::string(dot == std::string_view::npos ? id : id.substr(dot + 1));
}

int id_depth(std::string_view id) {
  if (id.empty()) return 0;
  return static_cast<int>(std::count(id.begin(), id.end(), '.')) + 1;
}

// --- Schema methods -------------------------------------------------------------------------

const Block* Schema::find(std::string_view id) const {
  auto it = std::lower_bound(blocks.begin(), blocks.end(), id,
                             [](const Block& b, std::string_view v) { return b.id < v; });
  if (it != blocks.end() && it->id == id) return &*it;
  return nullptr;
}

Block* Schema::find_mut(std::string_view id) {
  return const_cast<Block*>(static_cast<const Schema*>(this)->find(id));
}

std::vector<const Block*> Schema::children(std::string_view id) const {
  std::vector<const Block*> out;
  for (const auto& b : blocks) {
    if (b.parent == id) out.push_back(&b);
  }
  return out;
}

bool Schema::is_under(std::string_view ancestor, std::string_view id) {
  if (ancestor.empty() || ancestor == "root") return true;
  return id == ancestor || (id.size() > ancestor.size() && id.starts_with(ancestor) && id[ancestor.size()] == '.');
}

std::vector<const Block*> Schema::leaves_under(std::string_view id) const {
  std::vector<const Block*> out;
  for (const auto& b : blocks) {
    if (b.has_code() && is_under(id, b.id)) out.push_back(&b);
  }
  return out;
}

std::vector<std::string> Schema::ancestors(std::string_view id) const {
  std::vector<std::string> out;
  std::string p = parent_id(id);
  while (!p.empty()) {
    out.push_back(p);
    p = parent_id(p);
  }
  return out;
}

std::filesystem::path Schema::blocks_root() const {
  if (project.dirs.blocks == "." || project.dirs.blocks.empty()) return root;
  return root / project.dirs.blocks;
}

std::filesystem::path Schema::block_dir(std::string_view id) const {
  return blocks_root() / id_to_rel_path(id);
}

std::vector<LinkInfo> Schema::links() const {
  std::vector<LinkInfo> out;
  auto thread_of = [&](const std::string& id) {
    const Block* b = find(id);
    return b != nullptr ? b->thread.value : std::string();
  };
  for (const auto& b : blocks) {
    for (const auto& p : b.ports) {
      if (p.dir != PortDir::In || !p.from) continue;
      LinkInfo l;
      l.src_block = p.from->block;
      l.src_port = p.from->port;
      l.dst_block = b.id;
      l.dst_port = p.name;
      l.kind = p.link;
      l.via = p.via;
      l.sync = p.sync;
      l.rate = p.rate;
      l.declared_in = b.id;
      l.line = p.line;
      out.push_back(std::move(l));
    }
    for (const auto& c : b.calls) {
      LinkInfo l;
      l.src_block = b.id;
      l.dst_block = c.target.block;
      l.dst_port = c.target.port;
      l.kind = LinkKind::Call;
      l.via = c.via;
      l.sync = c.sync;
      l.rate = c.rate;
      l.declared_in = b.id;
      l.line = c.line;
      out.push_back(std::move(l));
    }
    for (const auto& u : b.uses) {
      LinkInfo l;
      l.src_block = b.id;
      l.dst_block = u.target;
      l.kind = LinkKind::Use;
      l.declared_in = b.id;
      l.line = u.line;
      out.push_back(std::move(l));
    }
    for (const auto& s : b.states) {
      LinkInfo l;
      l.src_block = b.id;
      l.src_port = s.name;
      l.dst_block = s.with;
      l.kind = LinkKind::State;
      l.via = s.via;
      l.sync = s.sync;
      l.declared_in = b.id;
      l.line = s.line;
      out.push_back(std::move(l));
    }
    // Notes: a [[link]] of the text names one note or topic (docs/adr/0038); one line per pair.
    std::set<std::string> referred;
    for (const auto& r : b.refs) {
      if (r.target.empty() || r.target == b.id || !referred.insert(r.target).second) continue;
      LinkInfo l;
      l.src_block = b.id;
      l.dst_block = r.target;
      l.kind = LinkKind::Ref;
      l.declared_in = b.id;
      l.line = r.line;
      out.push_back(std::move(l));
    }
  }
  for (auto& l : out) {
    l.thread_src = thread_of(l.src_block);
    l.thread_dst = thread_of(l.dst_block);
  }
  return out;
}

std::vector<std::string> Schema::leaf_flow_order() const {
  // Kahn's algorithm over leaf/entry blocks with data/event (src→dst) and call
  // (caller→callee) edges. Cycles are broken by taking the remaining node with the
  // fewest incoming edges (ties by id). Shared blocks go last, sorted by id.
  std::set<std::string> nodes;
  std::vector<std::string> shared;
  for (const auto& b : blocks) {
    if (!b.has_code()) continue;
    if (b.kind == BlockKind::Shared) {
      shared.push_back(b.id);
    } else {
      nodes.insert(b.id);
    }
  }
  std::map<std::string, std::set<std::string>> out_edges;
  std::map<std::string, int> indeg;
  for (const auto& n : nodes) indeg[n] = 0;
  for (const auto& l : links()) {
    if (l.kind != LinkKind::Data && l.kind != LinkKind::Event && l.kind != LinkKind::Call) continue;
    if (!nodes.contains(l.src_block) || !nodes.contains(l.dst_block) || l.src_block == l.dst_block) continue;
    if (out_edges[l.src_block].insert(l.dst_block).second) ++indeg[l.dst_block];
  }
  std::vector<std::string> order;
  std::set<std::string> done;
  std::priority_queue<std::string, std::vector<std::string>, std::greater<>> ready;
  for (const auto& [n, d] : indeg) {
    if (d == 0) ready.push(n);
  }
  while (order.size() < nodes.size()) {
    if (ready.empty()) {
      std::string best;
      int best_deg = 1 << 30;
      for (const auto& [n, d] : indeg) {
        if (!done.contains(n) && d < best_deg) {
          best = n;
          best_deg = d;
        }
      }
      indeg[best] = 0;
      ready.push(best);
    }
    std::string n = ready.top();
    ready.pop();
    if (done.contains(n)) continue;
    done.insert(n);
    order.push_back(n);
    for (const auto& m : out_edges[n]) {
      if (done.contains(m)) continue;
      if (--indeg[m] == 0) ready.push(m);
    }
  }
  std::sort(shared.begin(), shared.end());
  order.insert(order.end(), shared.begin(), shared.end());
  return order;
}

std::vector<const Block*> Schema::ordered_children(std::string_view id) const { return ordered_children(id, leaf_flow_order()); }

std::vector<const Block*> Schema::ordered_children(std::string_view id, const std::vector<std::string>& order) const {
  auto kids = children(id);
  std::map<std::string, std::size_t> rank;
  for (std::size_t i = 0; i < order.size(); ++i) rank[order[i]] = i;
  auto rank_of = [&](const Block* b) {
    std::size_t best = order.size() + 1;
    for (const auto* l : leaves_under(b->id)) {
      auto it = rank.find(l->id);
      if (it != rank.end()) best = std::min(best, it->second);
    }
    return best;
  };
  auto only_shared = [&](const Block* b) {
    if (b->kind == BlockKind::Shared) return true;
    auto leaves = leaves_under(b->id);
    return !leaves.empty() &&
           std::all_of(leaves.begin(), leaves.end(), [](const Block* x) { return x->kind == BlockKind::Shared; });
  };
  std::vector<std::tuple<int, std::size_t, std::string, const Block*>> keyed;
  for (const auto* k : kids) {
    int cls = k->kind == BlockKind::Entry ? 0 : (only_shared(k) ? 2 : 1);
    keyed.emplace_back(cls, rank_of(k), k->id, k);
  }
  std::sort(keyed.begin(), keyed.end());
  std::vector<const Block*> out;
  for (auto& t : keyed) out.push_back(std::get<3>(t));
  return out;
}

// --- loading ------------------------------------------------------------------------------------

namespace {

Issue issue_from_error(const Error& e, std::string block) {
  return Issue{e.code, 'E', std::move(block), e.message, e.loc};
}

bool skip_dir(const std::string& name, const Dirs& dirs) {
  if (name.empty() || name[0] == '.') return true;
  return name == dirs.api || name == dirs.impl || name == dirs.tests || name == dirs.mocks || name == "_deps" ||
         name == "node_modules";
}

std::vector<std::filesystem::path> sorted_subdirs(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_directory(ec)) out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

void walk(Schema& s, const std::filesystem::path& dir, const std::string& prefix, int depth) {
  for (const auto& sub : sorted_subdirs(dir)) {
    std::string name = sub.filename().string();
    if (skip_dir(name, s.project.dirs) || is_build_dir(sub)) continue;
    auto card = sub / "block.ksl";
    std::string rel_dir = rel_str(s.root, sub);
    if (!std::filesystem::exists(card)) {
      if (std::filesystem::exists(sub / "project.ksl")) continue;  // another project (fixtures)
      for (const auto& inner : sorted_subdirs(sub)) {
        if (std::filesystem::exists(inner / "block.ksl")) {
          s.issues.push_back(Issue{"E01", 'E', "",
                                   fmt::format("folder '{}' holds blocks but has no block.ksl", rel_dir),
                                   SourceLoc{rel_dir, 0, 0}});
          break;
        }
      }
      continue;
    }
    std::string id = prefix.empty() ? name : prefix + "." + name;
    std::string display = rel_str(s.root, card);
    auto text = read_file(card);
    if (!text) {
      s.issues.push_back(Issue{"io", 'E', id, text.error().message, SourceLoc{display, 0, 0}});
      continue;
    }
    auto block = parse_block_text(*text, display);
    if (!block) {
      s.issues.push_back(issue_from_error(block.error(), id));
      walk(s, sub, id, depth + 1);
      continue;
    }
    if (!is_ident(name)) {
      s.issues.push_back(Issue{"E01", 'E', id,
                               fmt::format("folder name '{}' is not a valid block identifier", name),
                               SourceLoc{display, block->line, 1}});
    } else if (block->id != id) {
      s.issues.push_back(Issue{"E01", 'E', id,
                               fmt::format("id '{}' does not match folder path, expected '{}'", block->id, id),
                               SourceLoc{display, block->line, 8}});
    }
    block->id = id;
    block->dir = rel_dir;
    block->parent = prefix;
    block->depth = depth;
    s.blocks.push_back(std::move(*block));
    walk(s, sub, id, depth + 1);
  }
}

}  // namespace

Result<Block> load_block_file(const std::filesystem::path& file, std::string_view display) {
  auto text = read_file(file);
  if (!text) return std::unexpected(text.error());
  return parse_block_text(*text, display);
}

Result<Schema> load_schema(const std::filesystem::path& root_in) {
  Schema s;
  s.root = normalize(root_in);
  auto pfile = s.root / "project.ksl";
  if (!std::filesystem::exists(pfile)) {
    return fail("io", fmt::format("project.ksl not found in {}", s.root.string()));
  }
  auto text = read_file(pfile);
  if (!text) return std::unexpected(text.error());
  auto project = parse_project_text(*text, "project.ksl");
  if (!project) return std::unexpected(project.error());
  s.project = std::move(*project);
  auto broot = s.blocks_root();
  if (std::filesystem::is_directory(broot)) walk(s, broot, "", 1);
  std::sort(s.blocks.begin(), s.blocks.end(), [](const Block& a, const Block& b) { return a.id < b.id; });
  for (auto& b : s.blocks) {
    if (b.parent.empty()) continue;
    if (Block* p = s.find_mut(b.parent)) p->children.push_back(b.id);
  }
  // Notes: the links of each note's text, resolved once every block is known (docs/adr/0038). A note's
  // own text is note.md; a note written before notes were Markdown keeps its note.txt (docs/adr/0040).
  if (is_notes_project(s.project)) {
    std::map<std::string, std::string> by_file;  // note file → its note
    MediaFinder media(s.root);
    for (auto& b : s.blocks) {
      if (b.kind == BlockKind::Leaf && !b.external()) {
        std::error_code ec;
        b.note_txt = !std::filesystem::exists(s.root / b.dir / std::string(kNoteFileName), ec) &&
                     std::filesystem::exists(s.root / b.dir / std::string(kOldNoteFileName), ec);
      }
      auto file = note_file(b);
      if (file.empty()) continue;
      by_file[file] = b.id;
      auto note = read_file(s.root / file);
      if (!note) continue;  // not written yet
      b.refs = parse_note_refs(*note);
      if (md::is_markdown_file(file)) {
        for (const auto& l : md::links(*note)) {
          if (auto path = note_link_file(file, l.href)) b.refs.push_back(NoteRef{.name = l.href, .line = l.line, .target = {}, .matches = 0, .path = *path});
        }
        std::stable_sort(b.refs.begin(), b.refs.end(), [](const NoteRef& x, const NoteRef& y) { return x.line < y.line; });
        for (const auto& im : md::images(*note)) {  // local images and the files they show (docs/adr/0041)
          if (!im.wiki && md::is_url(im.src)) continue;
          b.media.push_back(NoteMedia{.src = im.src, .wiki = im.wiki, .line = im.line, .file = media.find(file, im.src, im.wiki).value_or("")});
        }
      }
    }
    for (auto& b : s.blocks) {
      for (auto& r : b.refs) {
        if (!r.path.empty()) {
          auto it = by_file.find(r.path);
          r.matches = it == by_file.end() ? 0 : 1;
          if (it != by_file.end()) r.target = it->second;
          continue;
        }
        auto fits = resolve_note(s, r.name);
        r.matches = static_cast<int>(fits.size());
        if (fits.size() == 1) r.target = fits.front();
      }
    }
  }
  return s;
}

}  // namespace kk::core
