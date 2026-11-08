#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

#include "kk/core/paths.hpp"
#include "kk/index/hash.hpp"
#include "kk/layout/layout.hpp"

namespace kk::layout {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string graph_hash(const Graph& g) {
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto& n : g.nodes) items.emplace_back("n:" + n.id, fmt::format("{:.0f}x{:.0f}", n.w, n.h));
  for (const auto& e : g.edges) items.emplace_back("e:" + e.from + "->" + e.to + ":" + e.kind, "");
  items.emplace_back("sep", fmt::format("{:.0f}/{:.0f}", g.node_sep, g.rank_sep));
  return index::combine_hashes(std::move(items));
}

namespace {

bool shared_only(const index::Snapshot& s, const index::BlockRow& b) {
  if (b.kind == "shared") return true;
  if (b.kind != "group") return false;
  bool any = false;
  for (const auto& d : s.blocks) {
    if (d.id == b.id || !index::Snapshot::under(b.id, d.id) || d.kind == "group") continue;
    if (d.kind != "shared") return false;
    any = true;
  }
  return any;
}

std::string child_of(const std::string& scope, const std::string& id) {
  if (!index::Snapshot::under(scope, id)) return {};
  std::string rest = id;
  if (!scope.empty()) {
    if (id.size() == scope.size()) return {};
    rest = id.substr(scope.size() + 1);
  }
  auto head = rest.substr(0, rest.find('.'));
  return scope.empty() ? head : scope + "." + head;
}

std::vector<std::string> visible(const index::Snapshot& snap, const std::string& scope, bool show_use) {
  std::vector<std::string> out;
  for (const auto& c : snap.children_of(scope)) {
    const auto* b = snap.block(c);
    if (b == nullptr) continue;
    if (!show_use && scope.empty() && shared_only(snap, *b)) continue;
    out.push_back(c);
  }
  return out;
}

void add_edges(const index::Snapshot& snap, const std::string& scope, bool show_use, const std::set<std::string>& ids, Graph& g) {
  std::set<std::tuple<std::string, std::string, std::string>> seen;
  for (const auto& l : snap.links) {
    if (l.kind == "use" && !show_use) continue;
    auto a = child_of(scope, l.src_block);
    auto b = child_of(scope, l.dst_block);
    if (a.empty() || b.empty() || a == b || !ids.contains(a) || !ids.contains(b)) continue;
    if (seen.insert({a, b, l.kind}).second) g.edges.push_back({a, b, l.kind});
  }
}

}  // namespace

Graph level_graph(const index::Snapshot& snap, const std::string& scope_in, bool show_use, const Measure& measure) {
  std::string scope = scope_in == "root" ? "" : scope_in;
  Graph g;
  std::set<std::string> ids;
  for (const auto& id : visible(snap, scope, show_use)) {
    auto [w, h] = measure(*snap.block(id));
    g.nodes.push_back({id, w, h});
    ids.insert(id);
  }
  add_edges(snap, scope, show_use, ids, g);
  return g;
}

std::vector<Box> layout_nested(const index::Snapshot& snap, const std::string& scope_in,
                               const std::function<bool(const std::string&)>& expand, bool show_use,
                               const Measure& measure, ILayoutEngine& engine) {
  std::function<std::pair<std::pair<double, double>, std::vector<Box>>(const std::string&, int)> rec;
  rec = [&](const std::string& scope, int depth) -> std::pair<std::pair<double, double>, std::vector<Box>> {
    Graph g;
    std::set<std::string> ids;
    std::map<std::string, std::vector<Box>> inner;
    for (const auto& id : visible(snap, scope, show_use)) {
      const auto* b = snap.block(id);
      if (b->kind == "group" && !snap.children_of(id).empty() && expand(id)) {
        auto [size, boxes] = rec(id, depth + 1);
        g.nodes.push_back({id, size.first + 2 * kFramePad, size.second + kFrameTitle + kFramePad});
        inner[id] = std::move(boxes);
      } else {
        auto [w, h] = measure(*b);
        g.nodes.push_back({id, w, h});
      }
      ids.insert(id);
    }
    add_edges(snap, scope, show_use, ids, g);
    auto l = engine.layout(g);
    if (!l) l = SugiyamaEngine().layout(g);
    std::vector<Box> out;
    for (const auto& n : g.nodes) {
      auto p = l->pos[n.id];
      bool frame = inner.contains(n.id);
      out.push_back({n.id, p.x, p.y, n.w, n.h, depth, frame});
      for (auto bx : inner[n.id]) {
        bx.x += p.x + kFramePad;
        bx.y += p.y + kFrameTitle;
        out.push_back(bx);
      }
    }
    return {{l->width, l->height}, out};
  };
  return rec(scope_in == "root" ? "" : scope_in, 0).second;
}

std::vector<Box> layout_all_levels(const index::Snapshot& snap, bool show_use, const Measure& measure, ILayoutEngine& engine) {
  return layout_nested(snap, "", [](const std::string&) { return true; }, show_use, measure, engine);
}

std::optional<Layout> LayoutStore::cached(const std::string& hash) const {
  auto p = root_ / ".karkas" / "cache" / "layout" / (hash + ".json");
  if (!fs::exists(p)) return std::nullopt;
  auto text = core::read_file(p);
  if (!text) return std::nullopt;
  try {
    auto j = json::parse(*text);
    Layout l;
    l.engine = j.value("engine", "");
    l.width = j.value("width", 0.0);
    l.height = j.value("height", 0.0);
    for (const auto& [k, v] : j["pos"].items()) l.pos[k] = Pos{v.at(0).get<double>(), v.at(1).get<double>()};
    return l;
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

void LayoutStore::store(const std::string& hash, const Layout& l) const {
  json pos = json::object();
  for (const auto& [k, p] : l.pos) pos[k] = json::array({p.x, p.y});
  json j = {{"engine", l.engine}, {"width", l.width}, {"height", l.height}, {"pos", pos}};
  (void)core::write_file(root_ / ".karkas" / "cache" / "layout" / (hash + ".json"), j.dump());
}

std::map<std::string, Pos> LayoutStore::manual(const std::string& view) const {
  std::map<std::string, Pos> out;
  auto p = root_ / ".karkas" / "layout" / ((view.empty() ? "root" : view) + ".json");
  auto text = core::read_file(p);
  if (!text) return out;
  try {
    for (const auto& [k, v] : json::parse(*text).items()) {
      Pos pos{v.at(0).get<double>(), v.at(1).get<double>()};
      if (v.size() >= 4) {
        pos.w = v.at(2).get<double>();
        pos.h = v.at(3).get<double>();
      }
      out[k] = pos;
    }
  } catch (const json::exception&) {
  }
  return out;
}

kk::VoidResult LayoutStore::save_manual(const std::string& view, const std::map<std::string, Pos>& pos) const {
  json j = json::object();
  for (const auto& [k, p] : pos) {
    if (p.w > 0 && p.h > 0) {
      j[k] = json::array({std::round(p.x), std::round(p.y), std::round(p.w), std::round(p.h)});
    } else {
      j[k] = json::array({std::round(p.x), std::round(p.y)});
    }
  }
  return core::write_file(root_ / ".karkas" / "layout" / ((view.empty() ? "root" : view) + ".json"), j.dump(2) + "\n");
}

Layout LayoutStore::get(const std::string& view, const Graph& g, ILayoutEngine& engine) const {
  auto hash = graph_hash(g) + "-" + engine.name();
  Layout l;
  if (auto c = cached(hash)) {
    l = *c;
  } else {
    auto r = engine.layout(g);
    if (!r) r = SugiyamaEngine().layout(g);
    l = *r;
    store(hash, l);
  }
  for (const auto& [id, p] : manual(view)) {
    if (!l.pos.contains(id)) continue;
    l.pos[id].x = p.x;
    l.pos[id].y = p.y;
  }
  for (const auto& n : g.nodes) {
    const auto& p = l.pos[n.id];
    l.width = std::max(l.width, p.x + n.w + 20);
    l.height = std::max(l.height, p.y + n.h + 20);
  }
  return l;
}

}  // namespace kk::layout
