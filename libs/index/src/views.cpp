// Views for models (4.7) and schema queries (7.3), rendered from the index.
#include <fmt/format.h>

#include <filesystem>

#include <cctype>
#include <cmath>

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

namespace {

// "a, b, c +5": the first `n` names of a list.
std::string first_names(const std::vector<std::string>& names, std::size_t n) {
  std::vector<std::string> head(names.begin(), names.begin() + static_cast<long>(std::min(n, names.size())));
  std::string out = core::join(head, ", ");
  if (names.size() > n) out += fmt::format(" +{}", names.size() - n);
  return out;
}

// A card with many call lines (an imported block calls hundreds of functions; Endless Sky's PlayerInfo
// 407 lines, 16.7K characters) shows them in a view as one line: the neighbors section of the same view
// groups them per block with their signatures. The card file keeps every line (ADR 0029).
constexpr std::size_t kCardCallLines = 24;

std::string compact_card_calls(const std::string& text) {
  auto lines = core::split(text, '\n', false);
  std::size_t calls = 0;
  std::set<std::string> targets;
  for (const auto& l : lines) {
    if (!l.starts_with("call ")) continue;
    ++calls;
    auto target = core::trim(l.substr(5));
    targets.insert(std::string(target.substr(0, target.find(':'))));
  }
  if (calls <= kCardCallLines) return text;
  std::vector<std::string> kept;
  bool said = false;
  for (const auto& l : lines) {
    if (!l.starts_with("call ")) {
      kept.push_back(l);
    } else if (!said) {
      kept.push_back(fmt::format("call    ({} calls of {} blocks; the neighbors section groups them per block, block.ksl lists each)",
                                 calls, targets.size()));
      said = true;
    }
  }
  return core::join(kept, "\n");
}

}  // namespace

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
    std::string text = compact_card_calls(card_text_without_comments(*card));
    auto nl = text.find('\n');
    std::string out = fmt::format("@view {} level=2 status={}\n", b.id, b.status);
    out += nl == std::string::npos ? "" : text.substr(nl + 1);
    std::vector<std::vector<std::string>> rows;
    // Calls are grouped per neighbour: imported blocks have dozens of them (docs/adr/0017).
    std::map<std::string, std::vector<std::string>> callees;  // block -> signatures we call
    std::map<std::string, std::vector<std::string>> callers;  // block -> our functions it calls
    std::vector<std::string> callee_order, caller_order;      // as the links come (card order)
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
        if (!callees.contains(l.dst_block)) callee_order.push_back(l.dst_block);
        callees[l.dst_block].push_back(port_type(s, l.dst_block, l.dst_port));
      } else if (l.dst_block == b.id && l.kind == "call") {
        if (!callers.contains(l.src_block)) caller_order.push_back(l.src_block);
        callers[l.src_block].push_back(l.dst_port);
      } else if (l.kind == "state" && (l.src_block == b.id || l.dst_block == b.id)) {
        auto other = l.src_block == b.id ? l.dst_block : l.src_block;
        rows.push_back({other, "state", l.src_port, "", thread_of(other)});
      }
    }
    std::map<std::string, std::size_t> weight_of;  // "fn|id" / "call|id" -> links the row stands for
    for (const auto& id : callee_order) {
      const auto& sigs = callees[id];
      rows.push_back({id, "fn", sigs.size() == 1 ? sigs[0] : first_names(sigs, 6), "", thread_of(id)});
      weight_of["fn|" + id] = sigs.size();
    }
    for (const auto& id : caller_order) {
      rows.push_back({id, "call", first_names(callers[id], 4), "", thread_of(id)});
      weight_of["call|" + id] = callers[id].size();
    }
    // Suppliers first, then consumers, callees, callers, state partners.
    auto rank = [](const std::string& d) {
      static const std::vector<std::string> order = {"out", "in", "fn", "call", "state"};
      return std::find(order.begin(), order.end(), d) - order.begin();
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const auto& x, const auto& y) { return rank(x[1]) < rank(y[1]); });
    // An imported block calls dozens of blocks (Endless Sky's engine: 60 rows, 14K characters): past
    // 24 rows only the 20 most used neighbors are listed, in the same order (ADR 0028).
    std::string rest;
    if (constexpr std::size_t kRows = 24, kKept = 20; rows.size() > kRows) {
      auto weight = [&](const std::vector<std::string>& r) {
        auto it = weight_of.find(r[1] + "|" + r[0]);
        return it == weight_of.end() ? std::size_t{1} : it->second;
      };
      std::vector<std::size_t> idx(rows.size());
      for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
      std::stable_sort(idx.begin(), idx.end(), [&](std::size_t x, std::size_t y) { return weight(rows[x]) > weight(rows[y]); });
      std::vector<bool> keep(rows.size(), false);
      for (std::size_t i = 0; i < kKept; ++i) keep[idx[i]] = true;
      std::vector<std::vector<std::string>> kept;
      for (std::size_t i = 0; i < rows.size(); ++i) {
        if (keep[i]) kept.push_back(rows[i]);
      }
      rest = fmt::format("(+{} more linked blocks, used less)\n", rows.size() - kept.size());
      rows = std::move(kept);
    }
    if (!rows.empty()) {
      out += "--- neighbors\n" + table(rows) + rest;
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

  // A big group of imported code has hundreds of links (Endless Sky: ~157K characters for the two
  // groups an orchestrator viewed). Past kInternalRows internal links, calls between two blocks share a
  // line; past kViewRows external rows only the heaviest pairs are listed; small groups look as before
  // (ADR 0028).
  constexpr std::size_t kInternalRows = 8;
  constexpr std::size_t kViewRows = 24;
  constexpr std::size_t kExternalKept = 20;
  std::vector<std::vector<std::string>> internal;
  std::vector<std::vector<std::string>> lifted_rows;
  std::vector<const LinkRow*> internal_links;
  for (const auto& l : s.links) {
    if (l.kind == "use" && !o.show_use) continue;
    auto a = child_of(b.id, l.src_block);
    auto c = child_of(b.id, l.dst_block);
    if (a.empty() || c.empty() || a == c) continue;
    if (a == l.src_block && c == l.dst_block) internal_links.push_back(&l);
  }
  if (internal_links.size() <= kInternalRows) {
    for (const auto* l : internal_links) {
      internal.push_back({end_ref(l->src_block, l->src_port), arrow(l->kind), end_ref(l->dst_block, l->dst_port),
                          link_attrs(*l, true, true)});
    }
  } else {
    std::map<std::pair<std::string, std::string>, std::size_t> pair_row;  // plain calls of a pair -> row
    std::vector<std::vector<std::string>> fns;                             // per row: called ports
    for (const auto* l : internal_links) {
      auto attrs = link_attrs(*l, true, true);
      if (l->kind == "call" && attrs.empty()) {
        auto [it, fresh] = pair_row.try_emplace({l->src_block, l->dst_block}, internal.size());
        if (fresh) {
          internal.push_back({l->src_block, arrow(l->kind), l->dst_block, ""});
          fns.emplace_back();
        }
        fns[it->second].push_back(l->dst_port);
        continue;
      }
      internal.push_back({end_ref(l->src_block, l->src_port), arrow(l->kind), end_ref(l->dst_block, l->dst_port), attrs});
      fns.emplace_back();
    }
    for (std::size_t i = 0; i < internal.size(); ++i) {
      if (!fns[i].empty()) internal[i][2] = end_ref(internal[i][2], fns[i].size() == 1 ? fns[i][0] : first_names(fns[i], 3));
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
  // Calls between the same two blocks share one line, where the first of them was: "-call-> b:f, g, h +9"
  // (docs/adr/0017); single links look as before.
  std::map<std::string, std::pair<std::size_t, std::vector<std::string>>> calls;  // key -> (row, fns)
  for (const auto& l : s.links) {
    if (l.kind == "use" && !o.show_use) continue;
    bool src_in = Snapshot::under(b.id, l.src_block);
    bool dst_in = Snapshot::under(b.id, l.dst_block);
    if (src_in == dst_in) continue;
    auto attrs = link_attrs(l, true, true);
    if (l.kind == "call" && attrs.empty()) {
      auto key = std::string(dst_in ? "in" : "out") + "|" + l.src_block + "|" + l.dst_block;
      auto [it, fresh] = calls.try_emplace(key);
      if (fresh) {
        it->second.first = ext.size();
        ext.push_back({dst_in ? "in" : "out", l.src_block, arrow(l.kind), l.dst_block, ""});
      }
      it->second.second.push_back(l.dst_port);
      continue;
    }
    ext.push_back({dst_in ? "in" : "out", end_ref(l.src_block, l.src_port), arrow(l.kind),
                   end_ref(l.dst_block, l.dst_port), attrs});
  }
  std::vector<std::size_t> weight(ext.size(), 1);  // links a row stands for
  for (auto& [key, rf] : calls) {
    auto& row = ext[rf.first];
    row[1] = end_ref(row[1], "");
    row[3] = end_ref(row[3], rf.second.size() == 1 ? rf.second[0] : first_names(rf.second, 3));
    weight[rf.first] = rf.second.size();
  }
  std::string ext_rest;
  if (ext.size() > kViewRows) {
    // The heaviest pairs, in their original order; the rest as one count.
    std::vector<std::size_t> idx(ext.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t x, std::size_t y) { return weight[x] > weight[y]; });
    std::vector<bool> keep(ext.size(), false);
    for (std::size_t i = 0; i < kExternalKept; ++i) keep[idx[i]] = true;
    std::vector<std::vector<std::string>> kept;
    std::size_t rest_in = 0, rest_out = 0, rest_links = 0;
    for (std::size_t i = 0; i < ext.size(); ++i) {
      if (keep[i]) {
        kept.push_back(ext[i]);
        continue;
      }
      (ext[i][0] == "in" ? rest_in : rest_out) += 1;
      rest_links += weight[i];
    }
    ext = std::move(kept);
    ext_rest = fmt::format("(+{} more block pairs, {} links: {} in, {} out; neighbors({}) lists them)\n", rest_in + rest_out,
                           rest_links, rest_in, rest_out, b.id);
  }
  std::stable_sort(ext.begin(), ext.end(), [](const auto& x, const auto& y) { return x[0] < y[0]; });
  if (!ext.empty()) out += "--- external\n" + table(ext) + ext_rest;
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

namespace {

// Lowercase words of an identifier or a sentence: "getBallPosition" -> get ball position,
// "NET_RADIUS" -> net radius, "src/PhysicWorld.cpp" -> src physic world cpp.
std::vector<std::string> words_of(std::string_view text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) out.push_back(core::to_lower(cur));
    cur.clear();
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    bool alnum = std::isalnum(static_cast<unsigned char>(c)) != 0;
    if (!alnum) {
      flush();
      continue;
    }
    bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
    if (upper && !cur.empty()) {
      bool prev_lower = std::islower(static_cast<unsigned char>(cur.back())) != 0;
      bool next_lower = i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1])) != 0;
      if (prev_lower || (next_lower && std::isupper(static_cast<unsigned char>(cur.back())) != 0)) flush();
    }
    cur += c;
  }
  flush();
  return out;
}

std::string stem(std::string w) {
  auto cut = [&](std::string_view suf, std::size_t min) {
    if (w.size() >= min + suf.size() && w.ends_with(suf)) {
      w.resize(w.size() - suf.size());
      return true;
    }
    return false;
  };
  if (cut("ies", 3)) return w + "y";
  if (!cut("ing", 3) && !cut("ed", 3) && !cut("es", 4)) cut("s", 3);
  return w;
}

// Same word up to an inflection or a derivation ("walk" ~ "walkable", "replay" ~ "replays");
// never a plain prefix ("serve" is not "server", "net" is not "network").
bool same_word(const std::string& a, const std::string& b) {
  if (a == b) return true;
  const auto& s = a.size() < b.size() ? a : b;
  const auto& l = a.size() < b.size() ? b : a;
  if (s.size() < 4 || !l.starts_with(s)) return false;
  static const std::vector<std::string> tails = {"e", "er", "s", "es", "ed", "d", "ing", "able", "ible", "ion", "ation", "ment", "ity"};
  auto rest = l.substr(s.size());
  return std::find(tails.begin(), tails.end(), rest) != tails.end() && !(rest == "er" && s.size() < 6);
}

// General software synonyms (deterministic): a word of a symptom also looks for these.
const std::map<std::string, std::vector<std::string>>& synonyms() {
  static const std::map<std::string, std::vector<std::string>> m = [] {
    std::vector<std::vector<std::string>> groups = {
        {"setting", "option", "config", "configuration", "preference"},
        {"online", "network", "multiplayer", "remote"},
        {"save", "write", "store", "persist"},
        {"load", "read", "restore", "parse"},
        {"record", "recorder", "recording"},
        {"crash", "abort", "exception"},
        {"sound", "audio"},
        {"key", "keyboard", "input"},
        {"screen", "display", "render", "draw"},
        {"restart", "startup", "init"},
    };
    std::map<std::string, std::vector<std::string>> out;
    for (const auto& g : groups) {
      for (const auto& w : g) {
        for (const auto& o : g) {
          if (o != w) out[w].push_back(o);
        }
      }
    }
    return out;
  }();
  return m;
}

const std::set<std::string>& stop_words() {
  static const std::set<std::string> s = {
      "the", "and", "for", "but", "not", "are", "was", "with", "when", "what", "that", "this", "then", "than",
      "from", "into", "onto", "over", "only", "just", "also", "very", "some", "every", "each", "after", "before",
      "while", "does", "did", "doe", "has", "have", "had", "its", "they", "them", "their", "there", "here",
      "should", "would", "could", "will", "can", "cannot", "dont", "doesnt", "didnt", "isnt", "wont", "you",
      "your", "our", "his", "her", "him", "she", "one", "all", "any", "sometime", "always", "never", "again",
      "still", "even", "like", "look", "seem", "happen", "thing", "way", "fine", "bug", "report", "other",
      "other", "same", "much", "more", "most", "less", "both", "either", "about", "around", "because"};
  return s;
}

}  // namespace

std::string Index::locate(std::string_view text, int limit) {
  std::vector<std::string> query;
  for (auto& w : words_of(text)) {
    auto s = stem(w);
    if (s.size() < 3 || stop_words().contains(w) || stop_words().contains(s)) continue;
    if (std::find(query.begin(), query.end(), s) == query.end()) query.push_back(s);
  }
  if (query.empty()) return "(no words to look for)\n";
  // Each query word with its synonyms.
  std::vector<std::vector<std::string>> forms;
  for (const auto& q : query) {
    std::vector<std::string> f = {q};
    if (auto it = synonyms().find(q); it != synonyms().end()) {
      for (const auto& o : it->second) f.push_back(stem(o));
    }
    forms.push_back(f);
  }
  struct Cand {
    std::string id, purpose;
    std::vector<int> field;  // per query word: best field weight (id 4, file 3, symbol 3, purpose 2)
    std::vector<std::string> files;
    std::vector<std::pair<int, std::string>> symbols;
    double score = 0;
  };
  std::map<std::string, Cand> cands;
  auto mark = [&](Cand& c, const std::string& word, int weight) -> int {
    auto s = stem(word);
    int n = 0;
    for (std::size_t i = 0; i < forms.size(); ++i) {
      for (const auto& f : forms[i]) {
        if (same_word(f, s)) {
          c.field[i] = std::max(c.field[i], weight);
          ++n;
          break;
        }
      }
    }
    return n;
  };
  auto snap = snapshot();
  for (const auto& b : snap.blocks) {
    if (b.kind == "group") continue;
    auto& c = cands[b.id];
    c.id = b.id;
    c.purpose = b.purpose;
    c.field.assign(query.size(), 0);
    auto last = b.id.substr(b.id.rfind('.') == std::string::npos ? 0 : b.id.rfind('.') + 1);
    for (const auto& w : words_of(last)) mark(c, w, 4);
    for (const auto& w : words_of(b.purpose)) mark(c, w, 2);
  }
  {
    auto q = db_.q("SELECT path, block FROM files WHERE block IS NOT NULL ORDER BY path");
    while (q.step()) {
      auto it = cands.find(q.text(1));
      if (it == cands.end()) continue;
      std::string path = q.text(0);
      it->second.files.push_back(path);
      for (const auto& w : words_of(std::filesystem::path(path).stem().string())) mark(it->second, w, 3);
    }
  }
  {
    auto q = db_.q("SELECT block, name, file, line FROM symbols WHERE block IS NOT NULL");
    while (q.step()) {
      auto it = cands.find(q.text(0));
      if (it == cands.end()) continue;
      int n = 0;
      for (const auto& w : words_of(q.text(1))) n += mark(it->second, w, 3);
      if (n > 0) it->second.symbols.emplace_back(n, fmt::format("{} ({}:{})", q.text(1), q.text(2), q.i64(3)));
    }
  }
  // A word found in few blocks says more than one found everywhere (inverse document frequency).
  const double total = static_cast<double>(std::max<std::size_t>(cands.size(), 1));
  std::vector<double> idf(query.size(), 0.0);
  for (std::size_t i = 0; i < query.size(); ++i) {
    int df = 0;
    for (const auto& [id, c] : cands) df += c.field[i] > 0 ? 1 : 0;
    idf[i] = df == 0 ? 0.0 : std::log(1.0 + total / df);
  }
  std::vector<Cand*> ranked;
  for (auto& [id, c] : cands) {
    // The three strongest words decide: two rare exact words beat six common weak ones.
    std::vector<double> parts;
    for (std::size_t i = 0; i < query.size(); ++i) {
      if (c.field[i] > 0) parts.push_back(idf[i] * c.field[i]);
    }
    if (parts.empty()) continue;
    std::sort(parts.rbegin(), parts.rend());
    for (std::size_t k = 0; k < parts.size() && k < 3; ++k) c.score += parts[k];
    ranked.push_back(&c);
  }
  if (ranked.empty()) return fmt::format("(no block matches: {})\n", core::join(query, " "));
  std::stable_sort(ranked.begin(), ranked.end(), [](const Cand* a, const Cand* b) { return a->score > b->score; });
  std::string out = fmt::format("@locate {}\n", core::join(query, " "));
  for (int i = 0; i < limit && i < static_cast<int>(ranked.size()); ++i) {
    auto& c = *ranked[static_cast<std::size_t>(i)];
    std::vector<std::string> hit;
    for (std::size_t k = 0; k < query.size(); ++k) {
      if (c.field[k] > 0) hit.push_back(query[k]);
    }
    out += fmt::format("{}. {}  score {:.0f}  ({})\n   {}\n", i + 1, c.id, 10.0 * c.score, core::join(hit, " "), c.purpose);
    if (!c.files.empty()) {
      std::vector<std::string> f(c.files.begin(), c.files.begin() + static_cast<long>(std::min<std::size_t>(c.files.size(), 4)));
      out += "   files " + core::join(f, " ") + (c.files.size() > 4 ? fmt::format(" (+{})", c.files.size() - 4) : "") + "\n";
    }
    std::stable_sort(c.symbols.begin(), c.symbols.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> sy;
    for (std::size_t k = 0; k < c.symbols.size() && k < 5; ++k) sy.push_back(c.symbols[k].second);
    if (!sy.empty()) out += "   symbols " + core::join(sy, ", ") + "\n";
  }
  return out;
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
    // The state of the contract, so an agent above knows who may change api/ (ADR 0020).
    if (!b.api_hash.empty()) {
      bool any_frozen = std::any_of(s.blocks.begin(), s.blocks.end(), [](const BlockRow& x) { return !x.api_frozen_hash.empty(); });
      if (!b.api_frozen_hash.empty()) {
        out += "api frozen (declarations are the contract; its worker may tune values of constants and add new constants)\n";
      } else if (any_frozen) {
        // D04 E: the card names a fn api/ lacks (api/ is behind). D04 W: both have it, with other
        // parameter counts — either side may be the right one, so no "behind" (T-0013: api/ had
        // gained a defaulted parameter and the card followed it).
        std::vector<std::string> differ;
        bool behind = false;
        for (const auto& d : s.drift) {
          if (d.block != b.id || d.code != "D04") continue;
          if (d.severity == "E") behind = true;
          else differ.push_back(d.message);
        }
        if (behind) {
          out += "api unfrozen by an approved change and behind its card (the card names what api/ lacks): its worker brings api/ "
                 "in line in a ticket\n";
        } else if (!differ.empty()) {
          out += fmt::format("api unfrozen by an approved change; api/ and its card differ ({}): api/ follows the card, or a change "
                             "brings the card in line when api/ is right\n",
                             core::join(differ, "; "));
        } else {
          out += "api unfrozen by an approved change, already in line with its card (a human freezes it: karkas approve api)\n";
        }
      }
    }
    for (const auto& t : s.tests) {
      if (!Snapshot::under(block, t.block)) continue;
      std::string line = fmt::format("tests {}", t.block);
      if (!t.fresh) line += " stale (the code or a linked api/ changed since this run; run_tests gives fresh results):";
      if (t.contract.total > 0) line += fmt::format(" contract {}/{}", t.contract.pass, t.contract.total);
      if (t.unit.total > 0) line += fmt::format(" unit {}/{}", t.unit.pass, t.unit.total);
      if (t.integration.total > 0) line += fmt::format(" integration {}/{}", t.integration.pass, t.integration.total);
      if (t.build_failed) line += " build=failed";
      out += line + "\n";
      for (const auto& f : t.failures) {
        if (!f.empty()) out += (t.fresh ? "fail " : "old fail ") + f + "\n";
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
