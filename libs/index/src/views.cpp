// Views for models (4.7) and schema queries (7.3), rendered from the index.
#include <fmt/format.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"

namespace kk::index {

namespace {

using core::pad_right;

// Column-aligned table; last column is not padded.
std::string table(const std::vector<std::vector<std::string>>& rows) {
  std::vector<std::size_t> width;
  for (const auto& r : rows) {
    for (std::size_t i = 0; i < r.size(); ++i) {
      if (width.size() <= i) width.push_back(0);
      width[i] = std::max(width[i], r[i].size());
    }
  }
  std::string out;
  for (const auto& r : rows) {
    std::string line;
    // Index of the last non-empty cell: trailing empty cells are not padded.
    std::size_t last = 0;
    for (std::size_t i = 0; i < r.size(); ++i) {
      if (!r[i].empty()) last = i;
    }
    for (std::size_t i = 0; i <= last && i < r.size(); ++i) {
      if (i == last) {
        line += r[i];
      } else {
        line += r[i];
        line.append(width[i] - r[i].size() + 2, ' ');
      }
    }
    out += line + "\n";
  }
  return out;
}

std::string status_text(const BlockRow& b) {
  if (b.status == "passing") return "ok";
  if (b.status == "failing") return b.failed_tests > 0 ? fmt::format("FAIL {}", b.failed_tests) : "FAIL";
  if (b.status == "drift") return b.drift_errors > 0 ? fmt::format("drift {}", b.drift_errors) : "drift";
  if (b.status == "needs_review") return "review";
  return b.status;
}

bool is_shared_only(const Snapshot& s, const BlockRow& b) {
  if (b.kind == "shared") return true;
  if (b.kind != "group") return false;
  bool any = false;
  for (const auto& d : s.blocks) {
    if (d.id == b.id || !Snapshot::under(b.id, d.id) || d.kind == "group") continue;
    if (d.kind != "shared") return false;
    any = true;
  }
  return any;
}

// The child of `scope` ("" = root) that contains `id`, or "" when outside.
std::string child_of(std::string_view scope, const std::string& id) {
  if (!Snapshot::under(scope, id)) return {};
  std::string_view rest = id;
  if (!scope.empty() && scope != "root") {
    if (id.size() == scope.size()) return {};
    rest = std::string_view(id).substr(scope.size() + 1);
  }
  auto dot = rest.find('.');
  std::string head(rest.substr(0, dot));
  if (scope.empty() || scope == "root") return head;
  return std::string(scope) + "." + head;
}

std::string thr(const LinkRow& l) {
  if (l.thread_src.empty() || l.thread_dst.empty() || l.thread_src == l.thread_dst) return {};
  return "thr=" + l.thread_src + "->" + l.thread_dst;
}

std::string link_attrs(const LinkRow& l, bool with_via, bool with_rate) {
  std::vector<std::string> a;
  if (with_via && !l.via.empty()) a.push_back(core::format_attr("via", l.via));
  if (!l.sync.empty()) a.push_back(core::format_attr("sync", l.sync));
  if (with_rate && !l.rate.empty()) a.push_back(core::format_attr("rate", l.rate));
  auto t = thr(l);
  if (!t.empty()) a.push_back(t);
  return core::join(a, " ");
}

std::string end_ref(const std::string& block, const std::string& port) {
  return port.empty() ? block : block + ":" + port;
}

std::string arrow(const std::string& kind) { return "-" + kind + "->"; }

struct Lifted {
  std::string src, dst, kind;
  int count = 0;
  std::set<std::string> syncs, thrs;
};

// Links between different children of `scope`, collapsed with a counter (3.6).
std::vector<Lifted> lift(const Snapshot& s, std::string_view scope, const ViewOptions& o,
                         const std::vector<std::string>& order) {
  std::map<std::tuple<std::string, std::string, std::string>, Lifted> m;
  for (const auto& l : s.links) {
    if (l.kind == "use" && !o.show_use) continue;
    auto a = child_of(scope, l.src_block);
    auto b = child_of(scope, l.dst_block);
    if (a.empty() || b.empty() || a == b) continue;
    if (std::find(order.begin(), order.end(), a) == order.end() ||
        std::find(order.begin(), order.end(), b) == order.end()) {
      continue;  // hidden child (shared when use links are off)
    }
    auto& x = m[{a, b, l.kind}];
    x.src = a;
    x.dst = b;
    x.kind = l.kind;
    ++x.count;
    x.syncs.insert(l.sync);
    x.thrs.insert(thr(l));
  }
  std::vector<Lifted> out;
  for (auto& [_, v] : m) out.push_back(std::move(v));
  auto rank = [&](const std::string& id) {
    return static_cast<std::size_t>(std::find(order.begin(), order.end(), id) - order.begin());
  };
  std::sort(out.begin(), out.end(), [&](const Lifted& x, const Lifted& y) {
    return std::make_tuple(rank(x.src), rank(x.dst), x.kind) < std::make_tuple(rank(y.src), rank(y.dst), y.kind);
  });
  return out;
}

std::vector<std::string> lifted_cells(const Lifted& x) {
  std::vector<std::string> attrs;
  if (x.count > 1) attrs.push_back(fmt::format("×{}", x.count));
  if (x.syncs.size() == 1 && !x.syncs.begin()->empty()) attrs.push_back("sync=" + *x.syncs.begin());
  if (x.thrs.size() == 1 && !x.thrs.begin()->empty()) attrs.push_back(*x.thrs.begin());
  return {x.src, arrow(x.kind), x.dst, core::join(attrs, " ")};
}

std::vector<std::string> visible_children(const Snapshot& s, std::string_view id, const ViewOptions& o) {
  std::vector<std::string> out;
  for (const auto& c : s.children_of(id)) {
    const auto* b = s.block(c);
    if (b == nullptr) continue;
    if (!o.show_use && is_shared_only(s, *b) && (id.empty() || id == "root")) continue;
    out.push_back(c);
  }
  return out;
}

std::string purpose_cell(const Snapshot& s, const BlockRow& b) {
  if (b.kind == "group") return fmt::format("{} ({})", b.purpose, s.children_of(b.id).size());
  return b.purpose;
}

kk::Result<const BlockRow*> need_block(const Snapshot& s, std::string_view id) {
  const auto* b = s.block(id);
  if (b == nullptr) return kk::fail("query", fmt::format("unknown block '{}'", id));
  return b;
}

std::string port_type(const Snapshot& s, const std::string& block, const std::string& port) {
  const auto* p = s.port(block, port);
  if (p == nullptr) return "?";
  return p->dir == "fn" ? p->signature : p->type;
}

// Probe summaries on links touching `scope`.
std::vector<std::string> probe_lines(const Snapshot& s, std::string_view scope) {
  std::vector<std::string> out;
  for (const auto& p : s.probes) {
    auto arrow_pos = p.link.find("->");
    std::string a = arrow_pos == std::string::npos ? p.link : p.link.substr(0, arrow_pos);
    std::string b = arrow_pos == std::string::npos ? "" : p.link.substr(arrow_pos + 2);
    if (scope.empty() || Snapshot::under(scope, a) || Snapshot::under(scope, b)) out.push_back(p.str());
  }
  return out;
}

}  // namespace

// --- L0 ----------------------------------------------------------------------------------------

std::string Index::overview(const ViewOptions& o) {
  auto s = snapshot();
  std::string out = "@view root level=0\n";
  if (!s.idea.empty()) out += "idea " + s.idea + "\n";
  auto kids = visible_children(s, "", o);
  std::vector<std::vector<std::string>> rows;
  for (const auto& id : kids) {
    const auto* b = s.block(id);
    rows.push_back({b->id, b->kind, purpose_cell(s, *b), status_text(*b)});
  }
  out += table(rows);
  auto lifted = lift(s, "", o, kids);
  if (!lifted.empty()) {
    out += "--- links\n";
    std::vector<std::vector<std::string>> lr;
    for (const auto& x : lifted) lr.push_back(lifted_cells(x));
    out += table(lr);
  }
  return out;
}

// --- L1 / L2 ---------------------------------------------------------------------------------------

kk::Result<std::string> Index::view(std::string_view block, int level, const ViewOptions& o) {
  if (block.empty() || block == "root" || level == 0) {
    if (level != 0 && !block.empty() && block != "root") return kk::fail("query", "level 0 is the project view");
    return overview(o);
  }
  auto s = snapshot();
  auto bb = need_block(s, block);
  if (!bb) return std::unexpected(bb.error());
  const BlockRow& b = **bb;
  if (b.kind != "group" || level >= 2) {
    // L2: the card + neighbor port signatures.
    if (b.kind == "group") return kk::fail("query", fmt::format("{} is a group: use level 1", b.id));
    auto card_path = root_ / b.dir / "block.ksl";
    auto card = core::load_block_file(card_path, core::rel_str(root_, card_path));
    if (!card) return std::unexpected(card.error());
    std::string text = card_text_without_comments(*card);
    auto nl = text.find('\n');
    std::string out = fmt::format("@view {} level=2 status={}\n", b.id, b.status);
    out += nl == std::string::npos ? "" : text.substr(nl + 1);
    std::vector<std::vector<std::string>> rows;
    auto thread_of = [&](const std::string& id) {
      const auto* x = s.block(id);
      return x != nullptr && !x->thread.empty() ? "thr=" + x->thread : std::string();
    };
    for (const auto& l : s.links) {
      if (l.kind == "use") continue;
      if (l.dst_block == b.id && (l.kind == "data" || l.kind == "event")) {
        rows.push_back({l.src_block, "out", l.src_port, port_type(s, l.src_block, l.src_port), thread_of(l.src_block)});
      } else if (l.src_block == b.id && (l.kind == "data" || l.kind == "event")) {
        rows.push_back({l.dst_block, "in", l.dst_port, port_type(s, l.dst_block, l.dst_port), thread_of(l.dst_block)});
      } else if (l.src_block == b.id && l.kind == "call") {
        rows.push_back({l.dst_block, "fn", port_type(s, l.dst_block, l.dst_port), "", thread_of(l.dst_block)});
      } else if (l.dst_block == b.id && l.kind == "call") {
        rows.push_back({l.src_block, "call", l.dst_port, "", thread_of(l.src_block)});
      } else if (l.kind == "state" && (l.src_block == b.id || l.dst_block == b.id)) {
        auto other = l.src_block == b.id ? l.dst_block : l.src_block;
        rows.push_back({other, "state", l.src_port, "", thread_of(other)});
      }
    }
    // Suppliers first, then consumers, callees, callers, state partners.
    auto rank = [](const std::string& d) {
      static const std::vector<std::string> order = {"out", "in", "fn", "call", "state"};
      return std::find(order.begin(), order.end(), d) - order.begin();
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const auto& x, const auto& y) { return rank(x[1]) < rank(y[1]); });
    if (!rows.empty()) {
      out += "--- neighbors\n" + table(rows);
    }
    return out;
  }

  // L1: children, internal links, external links, probes.
  std::string out = fmt::format("@view {} level=1\n", b.id);
  out += "purpose " + b.purpose + "\n";
  auto kids = visible_children(s, b.id, o);
  std::vector<std::vector<std::string>> rows;
  for (const auto& id : kids) {
    const auto* c = s.block(id);
    rows.push_back({c->id, c->kind, purpose_cell(s, *c), c->thread.empty() ? "" : "thr=" + c->thread, status_text(*c)});
  }
  // Keep the status column aligned even when groups have no thread.
  for (auto& r : rows) {
    if (r[3].empty()) r[3] = "-";
  }
  out += table(rows);

  std::vector<std::vector<std::string>> internal;
  std::vector<std::vector<std::string>> lifted_rows;
  for (const auto& l : s.links) {
    if (l.kind == "use" && !o.show_use) continue;
    auto a = child_of(b.id, l.src_block);
    auto c = child_of(b.id, l.dst_block);
    if (a.empty() || c.empty() || a == c) continue;
    if (a == l.src_block && c == l.dst_block) {
      internal.push_back({end_ref(l.src_block, l.src_port), arrow(l.kind), end_ref(l.dst_block, l.dst_port),
                          link_attrs(l, true, true)});
    }
  }
  for (const auto& x : lift(s, b.id, o, kids)) {
    // Only links that involve a sub-group are shown lifted; leaf-to-leaf links are above.
    const auto* xs = s.block(x.src);
    const auto* xd = s.block(x.dst);
    if ((xs != nullptr && xs->kind == "group") || (xd != nullptr && xd->kind == "group")) {
      lifted_rows.push_back(lifted_cells(x));
    }
  }
  if (!internal.empty() || !lifted_rows.empty()) {
    out += "--- links\n" + table(internal) + table(lifted_rows);
  }
  std::vector<std::vector<std::string>> ext;
  for (const auto& l : s.links) {
    if (l.kind == "use" && !o.show_use) continue;
    bool src_in = Snapshot::under(b.id, l.src_block);
    bool dst_in = Snapshot::under(b.id, l.dst_block);
    if (src_in == dst_in) continue;
    ext.push_back({dst_in ? "in" : "out", end_ref(l.src_block, l.src_port), arrow(l.kind),
                   end_ref(l.dst_block, l.dst_port), link_attrs(l, true, true)});
  }
  std::stable_sort(ext.begin(), ext.end(), [](const auto& x, const auto& y) { return x[0] < y[0]; });
  if (!ext.empty()) out += "--- external\n" + table(ext);
  auto probes = probe_lines(s, b.id);
  if (!probes.empty()) {
    out += "--- probes\n";
    for (const auto& p : probes) out += p + "\n";
  }
  return out;
}

// --- other queries (7.3) -------------------------------------------------------------------------

kk::Result<std::string> Index::children(std::string_view block) {
  auto s = snapshot();
  if (!block.empty() && block != "root") {
    auto b = need_block(s, block);
    if (!b) return std::unexpected(b.error());
  }
  std::vector<std::vector<std::string>> rows;
  for (const auto& id : s.children_of(block)) {
    const auto* c = s.block(id);
    if (c == nullptr) continue;
    rows.push_back({c->id, c->kind, purpose_cell(s, *c), c->thread.empty() ? "-" : "thr=" + c->thread, status_text(*c)});
  }
  if (rows.empty()) return std::string("(no children)\n");
  return table(rows);
}

kk::Result<std::string> Index::neighbors(std::string_view block, std::string_view kind, std::string_view dir) {
  auto s = snapshot();
  auto b = need_block(s, block);
  if (!b) return std::unexpected(b.error());
  std::vector<std::vector<std::string>> rows;
  for (const auto& l : s.links) {
    if (!kind.empty() && l.kind != kind) continue;
    bool src_in = Snapshot::under(block, l.src_block);
    bool dst_in = Snapshot::under(block, l.dst_block);
    if (src_in == dst_in) continue;
    std::string d = dst_in ? "in" : "out";
    if (dir != "both" && !dir.empty() && dir != d) continue;
    rows.push_back({d, end_ref(l.src_block, l.src_port), arrow(l.kind), end_ref(l.dst_block, l.dst_port),
                    link_attrs(l, true, true)});
  }
  if (rows.empty()) return std::string("(no neighbors)\n");
  return table(rows);
}

std::vector<std::string> Index::dependent_blocks(std::string_view block) {
  auto s = snapshot();
  std::set<std::string> out;
  for (const auto& l : s.links) {
    // Who depends on `block`'s interface: consumers of its outputs, its callers,
    // its users (shared) and state partners.
    if ((l.kind == "data" || l.kind == "event") && Snapshot::under(block, l.src_block) &&
        !Snapshot::under(block, l.dst_block)) {
      out.insert(l.dst_block);
    }
    if ((l.kind == "call" || l.kind == "use") && Snapshot::under(block, l.dst_block) &&
        !Snapshot::under(block, l.src_block)) {
      out.insert(l.src_block);
    }
    if (l.kind == "state") {
      if (Snapshot::under(block, l.src_block) && !Snapshot::under(block, l.dst_block)) out.insert(l.dst_block);
      if (Snapshot::under(block, l.dst_block) && !Snapshot::under(block, l.src_block)) out.insert(l.src_block);
    }
  }
  return {out.begin(), out.end()};
}

kk::Result<std::string> Index::dependents(std::string_view block) {
  auto s = snapshot();
  auto b = need_block(s, block);
  if (!b) return std::unexpected(b.error());
  std::vector<std::vector<std::string>> rows;
  for (const auto& l : s.links) {
    std::string who;
    if ((l.kind == "data" || l.kind == "event") && Snapshot::under(block, l.src_block) &&
        !Snapshot::under(block, l.dst_block)) {
      who = l.dst_block;
    } else if ((l.kind == "call" || l.kind == "use" || l.kind == "state") && Snapshot::under(block, l.dst_block) &&
               !Snapshot::under(block, l.src_block)) {
      who = l.src_block;
    } else if (l.kind == "state" && Snapshot::under(block, l.src_block) && !Snapshot::under(block, l.dst_block)) {
      who = l.dst_block;
    }
    if (who.empty()) continue;
    rows.push_back({who, l.kind, end_ref(l.src_block, l.src_port) + " -> " + end_ref(l.dst_block, l.dst_port)});
  }
  if (rows.empty()) return std::string("(no dependents)\n");
  return table(rows);
}

kk::Result<std::string> Index::path(std::string_view from, std::string_view to) {
  auto s = snapshot();
  for (auto id : {from, to}) {
    auto b = need_block(s, id);
    if (!b) return std::unexpected(b.error());
  }
  // BFS over directed links (data/event/call; state both ways) from any leaf under
  // `from` to any leaf under `to`; falls back to undirected search.
  auto search = [&](bool undirected) -> std::vector<const LinkRow*> {
    std::map<std::string, std::vector<std::pair<std::string, const LinkRow*>>> g;
    for (const auto& l : s.links) {
      if (l.kind == "use") continue;
      g[l.src_block].emplace_back(l.dst_block, &l);
      if (undirected || l.kind == "state") g[l.dst_block].emplace_back(l.src_block, &l);
    }
    std::deque<std::string> q;
    std::map<std::string, std::pair<std::string, const LinkRow*>> prev;
    std::set<std::string> seen;
    for (const auto& b : s.blocks) {
      if (b.kind != "group" && Snapshot::under(from, b.id)) {
        q.push_back(b.id);
        seen.insert(b.id);
      }
    }
    while (!q.empty()) {
      auto cur = q.front();
      q.pop_front();
      if (Snapshot::under(to, cur) && !Snapshot::under(from, cur)) {
        std::vector<const LinkRow*> chain;
        for (auto at = cur; prev.contains(at); at = prev[at].first) chain.push_back(prev[at].second);
        std::reverse(chain.begin(), chain.end());
        return chain;
      }
      for (const auto& [next, l] : g[cur]) {
        if (seen.insert(next).second) {
          prev[next] = {cur, l};
          q.push_back(next);
        }
      }
    }
    return {};
  };
  auto chain = search(false);
  bool undirected = false;
  if (chain.empty()) {
    chain = search(true);
    undirected = true;
  }
  if (chain.empty()) return fmt::format("no path {} -> {}\n", from, to);
  std::string out = fmt::format("path {} -> {} hops={}{}\n", from, to, chain.size(), undirected ? " undirected" : "");
  std::vector<std::vector<std::string>> rows;
  for (const auto* l : chain) {
    rows.push_back({end_ref(l->src_block, l->src_port), arrow(l->kind), end_ref(l->dst_block, l->dst_port),
                    link_attrs(*l, true, true)});
  }
  return out + table(rows);
}

kk::Result<std::string> Index::writers(std::string_view portref) {
  auto ref = core::parse_port_ref(portref);
  if (!ref) return kk::fail("query", "expected <block>:<port>");
  auto s = snapshot();
  const auto* p = s.port(ref->block, ref->port);
  if (p == nullptr) return kk::fail("query", fmt::format("unknown port {}", portref));
  std::vector<std::vector<std::string>> rows;
  if (p->dir == "out") {
    rows.push_back({ref->block, "writes", ref->str(), p->type});
  }
  for (const auto& l : s.links) {
    if (p->dir == "in" && l.dst_block == ref->block && l.dst_port == ref->port) {
      rows.push_back({l.src_block, arrow(l.kind), end_ref(l.src_block, l.src_port), link_attrs(l, true, true)});
    }
    if (p->dir == "fn" && l.kind == "call" && l.dst_block == ref->block && l.dst_port == ref->port) {
      rows.push_back({l.src_block, "calls", ref->str(), link_attrs(l, true, true)});
    }
  }
  if (rows.empty()) return std::string("(no writers)\n");
  return table(rows);
}

kk::Result<std::string> Index::readers(std::string_view portref) {
  auto ref = core::parse_port_ref(portref);
  if (!ref) return kk::fail("query", "expected <block>:<port>");
  auto s = snapshot();
  const auto* p = s.port(ref->block, ref->port);
  if (p == nullptr) return kk::fail("query", fmt::format("unknown port {}", portref));
  std::vector<std::vector<std::string>> rows;
  if (p->dir == "in") rows.push_back({ref->block, "reads", ref->str(), p->type});
  for (const auto& l : s.links) {
    if (p->dir == "out" && l.src_block == ref->block && l.src_port == ref->port) {
      rows.push_back({l.dst_block, arrow(l.kind), end_ref(l.dst_block, l.dst_port), link_attrs(l, true, true)});
    }
  }
  if (rows.empty()) return std::string("(no readers)\n");
  return table(rows);
}

std::string Index::by_thread(std::string_view thread) {
  auto s = snapshot();
  std::vector<std::vector<std::string>> rows;
  for (const auto& b : s.blocks) {
    if (b.kind != "group" && b.thread == thread) rows.push_back({b.id, b.kind, b.purpose, status_text(b)});
  }
  if (rows.empty()) return fmt::format("(no blocks in thread {})\n", thread);
  return table(rows);
}

std::string Index::search(std::string_view text, int limit) {
  // Each word becomes a quoted FTS5 term (no query syntax injection); all words must match,
  // then fall back to any word.
  auto words = core::split_ws(text);
  if (words.empty()) return "(empty query)\n";
  std::vector<std::string> terms;
  for (auto w : words) terms.push_back("\"" + core::replace_all(w, "\"", "\"\"") + "\"");
  std::vector<std::vector<std::string>> rows;
  for (const auto* op : {" ", " OR "}) {
    auto q = db_.q(
        "SELECT block, purpose, snippet(search, -1, '[', ']', '...', 8) FROM search WHERE search MATCH ? ORDER BY rank "
        "LIMIT ?");
    q.bind(1, core::join(terms, op)).bind(2, limit);
    while (q.step()) {
      std::string snip = q.text(2);
      std::string purpose = q.text(1);
      rows.push_back({q.text(0), purpose, snip == purpose ? "" : snip});
    }
    if (!rows.empty() || terms.size() == 1) break;
  }
  if (rows.empty()) return "(no matches)\n";
  return table(rows);
}

kk::Result<std::string> Index::history(std::string_view block, int n) {
  auto s = snapshot();
  auto b = need_block(s, block);
  if (!b) return std::unexpected(b.error());
  std::vector<std::string> lines;
  for (const auto& [id, entries] : s.history) {
    if (!Snapshot::under(block, id)) continue;
    for (const auto& [t, text] : entries) {
      lines.push_back(id == block ? fmt::format("hist {} {}", t, text) : fmt::format("hist {} {} {}", id, t, text));
    }
  }
  if (lines.empty()) return std::string("(no history)\n");
  std::string out;
  std::size_t start = lines.size() > static_cast<std::size_t>(n) ? lines.size() - static_cast<std::size_t>(n) : 0;
  for (std::size_t i = start; i < lines.size(); ++i) out += lines[i] + "\n";
  return out;
}

kk::Result<std::string> Index::status(std::string_view block) {
  auto s = snapshot();
  std::string out;
  if (!block.empty() && block != "root") {
    auto bb = need_block(s, block);
    if (!bb) return std::unexpected(bb.error());
    const auto& b = **bb;
    out += fmt::format("status {} {}\n", b.id, b.status);
    for (const auto& t : s.tests) {
      if (!Snapshot::under(block, t.block)) continue;
      std::string line = fmt::format("tests {}", t.block);
      if (t.contract.total > 0) line += fmt::format(" contract {}/{}", t.contract.pass, t.contract.total);
      if (t.unit.total > 0) line += fmt::format(" unit {}/{}", t.unit.pass, t.unit.total);
      if (t.integration.total > 0) line += fmt::format(" integration {}/{}", t.integration.pass, t.integration.total);
      if (t.build_failed) line += " build=failed";
      if (!t.fresh) line += " stale";
      out += line + "\n";
      for (const auto& f : t.failures) {
        if (!f.empty()) out += "fail " + f + "\n";
      }
    }
    for (const auto& d : s.drift) {
      if (!d.block.empty() && Snapshot::under(block, d.block)) out += d.str() + "\n";
    }
    return out;
  }
  std::map<std::string, int> counts;
  for (const auto& b : s.blocks) {
    if (b.kind != "group") ++counts[b.status];
  }
  std::vector<std::string> parts;
  for (const auto& [k, v] : counts) parts.push_back(fmt::format("{}={}", k, v));
  out += "summary " + core::join(parts, " ") + "\n";
  std::vector<std::vector<std::string>> rows;
  for (const auto& b : s.blocks) {
    if (b.kind == "group" || b.status == "passing") continue;
    rows.push_back({b.id, status_text(b)});
  }
  out += table(rows);
  int errors = 0;
  for (const auto& d : s.drift) errors += d.severity == "E" ? 1 : 0;
  if (errors > 0) out += fmt::format("drift errors={}\n", errors);
  return out;
}

std::string Index::probes(std::string_view target) {
  auto s = snapshot();
  std::vector<std::string> lines;
  for (const auto& p : s.probes) {
    if (!target.empty()) {
      if (core::contains(target, "->")) {
        if (p.link != target) continue;
      } else {
        auto arrow_pos = p.link.find("->");
        std::string a = p.link.substr(0, arrow_pos);
        std::string b = arrow_pos == std::string::npos ? "" : p.link.substr(arrow_pos + 2);
        if (!Snapshot::under(target, a) && !Snapshot::under(target, b)) continue;
      }
    }
    lines.push_back("probe " + p.str());
  }
  if (lines.empty()) return "(no probe data)\n";
  return core::join(lines, "\n") + "\n";
}

std::string Index::drift(std::string_view block) {
  auto s = snapshot();
  std::string out;
  for (const auto& d : s.drift) {
    if (!block.empty() && (d.block.empty() || !Snapshot::under(block, d.block))) continue;
    out += d.str() + "\n";
  }
  return out.empty() ? "(no drift)\n" : out;
}

}  // namespace kk::index
