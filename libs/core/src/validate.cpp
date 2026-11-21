#include "kk/core/validate.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

#include "kk/core/notes.hpp"
#include "kk/core/text.hpp"

namespace kk::core {

namespace {

struct Checker {
  const Schema& s;
  std::vector<Issue> out;

  void add(std::string code, char sev, const Block* b, std::string msg, int line, int col) {
    SourceLoc loc;
    if (b != nullptr) loc = SourceLoc{b->file, line, col};
    out.push_back(Issue{std::move(code), sev, b != nullptr ? b->id : "", std::move(msg), std::move(loc)});
  }

  static bool kind_allowed(std::string_view kinds, const Block& b) {
    if (kinds.empty() || !b.kind) return true;
    for (const auto& k : split(kinds, ',')) {
      if (k == to_string(*b.kind)) return true;
    }
    return false;
  }

  void e08(const Block& b, std::string_view directive, int line, int col) {
    const auto* spec = find_directive("block", directive);
    if (spec == nullptr || kind_allowed(spec->kinds, b)) return;
    add("E08", 'E', &b, fmt::format("'{}' is not allowed in a {} block", directive, b.kind_name()), line, col);
  }

  // A notes project (docs/adr/0038): topics and notes, their titles, the [[links]] of the texts; none
  // of the checks of code (entry, ports, threads, guarantees, sizes of groups).
  void run_notes() {
    for (const auto& b : s.blocks) {
      if (!b.purpose.set) add("E02", 'E', &b, "missing title (purpose)", b.line, 1);
      if (!b.kind) add("E02", 'E', &b, "missing kind", b.line, 1);
      if (b.kind && *b.kind != BlockKind::Group && *b.kind != BlockKind::Leaf) {
        add("E10", 'E', &b, fmt::format("a notes project holds topics (group) and notes (leaf), not {}", b.kind_name()),
            b.kind_text.line, b.kind_text.col);
      }
      if (b.kind == BlockKind::Leaf && !b.children.empty()) {
        add("E06", 'E', &b, fmt::format("a note must not hold other blocks (found {})", b.children.front()), b.kind_text.line,
            b.kind_text.col);
      }
      // W08: a [[link]] that names no block, or several; a Markdown link to a file that is no note.
      for (const auto& r : b.refs) {
        if (r.matches == 1) continue;
        std::string msg = !r.path.empty()    ? fmt::format("the link ({}) names no note: {} is not a note's file", r.name, r.path)
                          : r.matches == 0 ? fmt::format("[[{}]] names no note", r.name)
                                           : fmt::format("[[{}]] names {} notes: write the id (topic.note)", r.name, r.matches);
        out.push_back(Issue{"W08", 'W', b.id, std::move(msg), SourceLoc{note_file(b), r.line, 1}});
      }
      // W09: an image whose file is not there (docs/adr/0041).
      for (const auto& im : b.media) {
        if (!im.file.empty()) continue;
        out.push_back(Issue{"W09", 'W', b.id, fmt::format("image {} not found", im.wiki ? "![[" + im.src + "]]" : im.src),
                            SourceLoc{note_file(b), im.line, 1}});
      }
    }
    files();
  }

  void run() {
    if (is_notes_project(s.project)) {
      run_notes();
      return;
    }
    std::size_t entries = 0;
    for (const auto& b : s.blocks) {
      // E02
      if (!b.purpose.set) add("E02", 'E', &b, "missing purpose", b.line, 1);
      if (!b.kind) add("E02", 'E', &b, "missing kind", b.line, 1);
      if (b.kind == BlockKind::Entry) ++entries;
      // E06 (tree shape)
      if (b.kind == BlockKind::Group && b.children.empty()) {
        add("E06", 'E', &b, "group has no children", b.kind_text.line, b.kind_text.col);
      }
      if (b.has_code() && !b.children.empty()) {
        add("E06", 'E', &b, fmt::format("{} block must not have children (found {})", b.kind_name(), b.children.front()),
            b.kind_text.line, b.kind_text.col);
      }
      // E08 (directive allowed for this kind)
      if (b.thread.set) e08(b, "thread", b.thread.line, b.thread.col);
      if (b.est.set) e08(b, "est", b.est.line, b.est.col);
      for (const auto& p : b.ports) e08(b, to_string(p.dir), p.line, p.col);
      for (const auto& c : b.calls) e08(b, "call", c.line, c.col);
      for (const auto& u : b.uses) e08(b, "use", u.line, u.col);
      for (const auto& f : b.files) e08(b, "files", f.line, f.col);
      for (const auto& st : b.states) e08(b, "state", st.line, st.col);
      for (const auto& i : b.invs) e08(b, "inv", i.line, i.col);
      for (const auto& h : b.hist) e08(b, "hist", h.line, 1);
      // E07
      std::set<std::string> names;
      for (const auto& p : b.ports) {
        if (!names.insert(p.name).second) {
          add("E07", 'E', &b, fmt::format("duplicate port name '{}'", p.name), p.line, p.col);
        }
      }
      refs(b);
      // W02
      for (const auto& st : b.states) {
        add("W02", 'W', &b, fmt::format("shared mutable state '{}' with {} (risk)", st.name, st.with), st.line, st.col);
      }
      // W03
      if (b.kind == BlockKind::Group && b.children.size() > 9) {
        add("W03", 'W', &b, fmt::format("group has {} children (more than 9)", b.children.size()), b.line, 1);
      }
      if (b.depth > 5) add("W03", 'W', &b, fmt::format("depth {} is more than 5", b.depth), b.line, 1);
      // W04
      if (b.purpose.set) {
        if (b.purpose.value.size() > 100) {
          add("W04", 'W', &b, fmt::format("purpose is {} characters long (more than 100)", b.purpose.value.size()),
              b.purpose.line, b.purpose.col);
        } else if (contains(b.purpose.value, " and ")) {
          add("W04", 'W', &b, "purpose contains \" and \": a block should have one responsibility",
              b.purpose.line, b.purpose.col);
        }
      }
      // W06 (imported code has no invariants written yet; they come later, from people)
      if (b.kind == BlockKind::Leaf && b.invs.empty() && !b.external()) add("W06", 'W', &b, "leaf has no inv", b.line, 1);
    }
    if (!s.blocks.empty() && entries != 1) {
      out.push_back(Issue{"E06", 'E', "", fmt::format("project must have exactly one entry block, found {}", entries),
                          SourceLoc{"project.ksl", s.project.line, 1}});
    }
    // W01: thread crossing without via=
    for (const auto& l : s.links()) {
      if (l.kind == LinkKind::Use || !l.crosses_threads() || !l.via.empty()) continue;
      const Block* b = s.find(l.declared_in);
      add("W01", 'W', b,
          fmt::format("{} link {} -> {} crosses threads {} -> {} without via=", to_string(l.kind), l.src_block,
                      l.dst_block, l.thread_src, l.thread_dst),
          l.line, 1);
    }
    files();
    call_cycles();
  }

  void refs(const Block& b) {
    for (const auto& p : b.ports) {
      if (p.dir != PortDir::In || !p.from) continue;
      const Block* t = s.find(p.from->block);
      if (t == nullptr) {
        add("E03", 'E', &b, fmt::format("from= refers to unknown block '{}'", p.from->block), p.line, p.col);
        continue;
      }
      const Port* tp = t->port(p.from->port);
      if (tp == nullptr) {
        add("E03", 'E', &b, fmt::format("block '{}' has no port '{}'", t->id, p.from->port), p.line, p.col);
      } else if (tp->dir != PortDir::Out) {
        add("E04", 'E', &b,
            fmt::format("{} link needs an out port, but {} is '{}'", to_string(p.link), p.from->str(),
                        to_string(tp->dir)),
            p.line, p.col);
      }
    }
    for (const auto& c : b.calls) {
      const Block* t = s.find(c.target.block);
      if (t == nullptr) {
        add("E03", 'E', &b, fmt::format("call refers to unknown block '{}'", c.target.block), c.line, c.col);
        continue;
      }
      const Port* tp = t->port(c.target.port);
      if (tp == nullptr) {
        add("E03", 'E', &b, fmt::format("block '{}' has no port '{}'", t->id, c.target.port), c.line, c.col);
      } else if (tp->dir != PortDir::Fn) {
        add("E05", 'E', &b, fmt::format("call target {} is '{}', not fn", c.target.str(), to_string(tp->dir)),
            c.line, c.col);
      }
    }
    for (const auto& u : b.uses) {
      const Block* t = s.find(u.target);
      if (t == nullptr) {
        add("E03", 'E', &b, fmt::format("use refers to unknown block '{}'", u.target), u.line, u.col);
      } else if (t->kind != BlockKind::Shared && !t->external()) {
        // Imported code (files) often includes another module only for its types: such a
        // "use" of a non-shared imported block is allowed (docs/adr/0009).
        add("E03", 'E', &b, fmt::format("use target '{}' is a {} block, not shared", u.target, t->kind_name()),
            u.line, u.col);
      }
    }
    for (const auto& st : b.states) {
      if (s.find(st.with) == nullptr) {
        add("E03", 'E', &b, fmt::format("state with= refers to unknown block '{}'", st.with), st.line, st.col);
      }
    }
  }

  // E09: files of imported blocks are claimed once and stay outside other blocks' folders.
  // W07: a listed file that does not exist yet. Schema first: a new block may name the files it
  // will be written in; it stays in design until they exist (docs/adr/0016).
  void files() {
    std::map<std::string, const Block*> owner;
    for (const auto& b : s.blocks) {
      for (const auto& f : b.files) {
        for (const auto& p : f.paths) {
          std::error_code ec;
          if (!s.root.empty() && !std::filesystem::is_regular_file(s.root / p, ec)) {
            add("W07", 'W', &b, fmt::format("files: '{}' does not exist yet", p), f.line, f.col);
          }
          if (auto [it, fresh] = owner.emplace(p, &b); !fresh) {
            add("E09", 'E', &b, fmt::format("files: '{}' already belongs to {}", p, it->second->id), f.line, f.col);
          }
          for (const auto& x : s.blocks) {
            if (&x != &b && x.has_code() && !x.dir.empty() && p.starts_with(x.dir + "/")) {
              add("E09", 'E', &b, fmt::format("files: '{}' is inside the folder of block {}", p, x.id), f.line, f.col);
            }
          }
        }
      }
    }
  }

  // W05: cycles of call links (Tarjan SCC).
  void call_cycles() {
    std::map<std::string, std::vector<std::string>> g;
    std::map<std::string, int> line_of;
    for (const auto& l : s.links()) {
      if (l.kind != LinkKind::Call || s.find(l.dst_block) == nullptr) continue;
      g[l.src_block].push_back(l.dst_block);
      g[l.dst_block];
    }
    std::map<std::string, int> index;
    std::map<std::string, int> low;
    std::set<std::string> on_stack;
    std::vector<std::string> stack;
    int counter = 0;
    std::function<void(const std::string&)> strong = [&](const std::string& v) {
      index[v] = low[v] = counter++;
      stack.push_back(v);
      on_stack.insert(v);
      for (const auto& w : g[v]) {
        if (!index.contains(w)) {
          strong(w);
          low[v] = std::min(low[v], low[w]);
        } else if (on_stack.contains(w)) {
          low[v] = std::min(low[v], index[w]);
        }
      }
      if (low[v] == index[v]) {
        std::vector<std::string> comp;
        std::string w;
        do {
          w = stack.back();
          stack.pop_back();
          on_stack.erase(w);
          comp.push_back(w);
        } while (w != v);
        bool self = std::find(g[v].begin(), g[v].end(), v) != g[v].end();
        if (comp.size() > 1 || self) {
          std::sort(comp.begin(), comp.end());
          const Block* b = s.find(comp.front());
          add("W05", 'W', b, fmt::format("call cycle between {}", join(comp, ", ")), b != nullptr ? b->line : 0, 1);
        }
      }
    };
    for (const auto& [v, _] : g) {
      if (!index.contains(v)) strong(v);
    }
  }
};

}  // namespace

std::vector<Issue> validate(const Schema& schema) {
  Checker c{schema, {}};
  c.run();
  return std::move(c.out);
}

std::vector<Issue> check_schema(const Schema& schema) {
  std::vector<Issue> all = schema.issues;
  auto v = validate(schema);
  all.insert(all.end(), v.begin(), v.end());
  std::stable_sort(all.begin(), all.end(), [](const Issue& a, const Issue& b) {
    return std::tie(a.loc.file, a.loc.line, a.loc.col, a.code) < std::tie(b.loc.file, b.loc.line, b.loc.col, b.code);
  });
  return all;
}

}  // namespace kk::core
