// The built-in layered layout (docs/adr/0002, improved in docs/adr/0042): blocks in ranks from left to
// right, the order inside a rank chosen for few crossings, positions that keep lines short and straight,
// and a route for every line through the gaps between blocks. Deterministic: the same graph gives the
// same layout (every tie goes by the input order).
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

#include "kk/layout/layout.hpp"

namespace kk::layout {

namespace {

using Edges = std::vector<std::pair<int, int>>;

// Crossings between two adjacent layers: edges as (position above, position below); an inversion count
// with a Fenwick tree (Barth, Jünger, Mutzel).
long crossings(std::vector<std::pair<int, int>> e, int below) {
  std::sort(e.begin(), e.end());
  std::vector<long> tree(static_cast<std::size_t>(below) + 1, 0);
  long count = 0;
  long seen = 0;
  for (const auto& [top, bottom] : e) {
    long not_after = 0;
    for (int i = bottom + 1; i > 0; i -= i & -i) not_after += tree[static_cast<std::size_t>(i)];
    count += seen - not_after;
    for (int i = bottom + 1; i <= below; i += i & -i) ++tree[static_cast<std::size_t>(i)];
    ++seen;
  }
  return count;
}

// Weighted isotonic regression (pool adjacent violators): the nondecreasing z closest to d.
std::vector<double> isotonic(const std::vector<double>& d, const std::vector<double>& w) {
  struct Pool {
    double sw, swd;
    std::size_t first, last;
  };
  std::vector<Pool> st;
  for (std::size_t i = 0; i < d.size(); ++i) {
    st.push_back({w[i], w[i] * d[i], i, i});
    while (st.size() >= 2 && st[st.size() - 2].swd / st[st.size() - 2].sw > st.back().swd / st.back().sw) {
      auto top = st.back();
      st.pop_back();
      st.back().sw += top.sw;
      st.back().swd += top.swd;
      st.back().last = top.last;
    }
  }
  std::vector<double> z(d.size());
  for (const auto& p : st) {
    for (std::size_t i = p.first; i <= p.last; ++i) z[i] = p.swd / p.sw;
  }
  return z;
}

struct Vertex {
  int node = -1;  // index of the real node, -1 for a dummy (a point of a long line)
  double w = 0, h = 0;
  int rank = 0;
  double y = 0;  // center
};

// One connected part laid out on its own.
struct Part {
  std::map<int, Pos> pos;                          // real node → top-left
  std::map<int, std::vector<Point>> chains;        // structural edge → its points (from its DAG tail)
  double width = 0, height = 0;
};

Part layout_part(const Graph& g, const std::vector<int>& nodes, const std::vector<std::pair<int, int>>& sedges,
                 const std::vector<int>& part_edges) {
  Part out;
  const int k = static_cast<int>(nodes.size());
  std::map<int, int> local;
  for (int i = 0; i < k; ++i) local[nodes[static_cast<std::size_t>(i)]] = i;
  // --- 1. Cycles: an order from the greedy heuristic of Eades, Lin and Smyth; edges against it turn round.
  std::vector<std::set<int>> out_n(static_cast<std::size_t>(k)), in_n(static_cast<std::size_t>(k));
  for (int e : part_edges) {
    int u = local[sedges[static_cast<std::size_t>(e)].first];
    int v = local[sedges[static_cast<std::size_t>(e)].second];
    out_n[static_cast<std::size_t>(u)].insert(v);
    in_n[static_cast<std::size_t>(v)].insert(u);
  }
  std::vector<int> head, tail;  // sources go to the head, sinks to the tail
  std::vector<bool> gone(static_cast<std::size_t>(k), false);
  int left = k;
  auto remove = [&](int v) {
    gone[static_cast<std::size_t>(v)] = true;
    --left;
    for (int u : in_n[static_cast<std::size_t>(v)]) out_n[static_cast<std::size_t>(u)].erase(v);
    for (int w : out_n[static_cast<std::size_t>(v)]) in_n[static_cast<std::size_t>(w)].erase(v);
  };
  while (left > 0) {
    bool changed = true;
    while (changed) {
      changed = false;
      for (int v = 0; v < k; ++v) {
        if (!gone[static_cast<std::size_t>(v)] && out_n[static_cast<std::size_t>(v)].empty()) {
          tail.push_back(v);
          remove(v);
          changed = true;
        }
      }
      for (int v = 0; v < k; ++v) {
        if (!gone[static_cast<std::size_t>(v)] && in_n[static_cast<std::size_t>(v)].empty()) {
          head.push_back(v);
          remove(v);
          changed = true;
        }
      }
    }
    if (left == 0) break;
    int best = -1;
    long best_d = 0;
    for (int v = 0; v < k; ++v) {
      if (gone[static_cast<std::size_t>(v)]) continue;
      long d = static_cast<long>(out_n[static_cast<std::size_t>(v)].size()) - static_cast<long>(in_n[static_cast<std::size_t>(v)].size());
      if (best < 0 || d > best_d) {
        best = v;
        best_d = d;
      }
    }
    head.push_back(best);
    remove(best);
  }
  std::vector<int> order_of(static_cast<std::size_t>(k));
  int o = 0;
  for (int v : head) order_of[static_cast<std::size_t>(v)] = o++;
  for (auto it = tail.rbegin(); it != tail.rend(); ++it) order_of[static_cast<std::size_t>(*it)] = o++;
  struct DagEdge {
    int u, v, e;  // local tail and head, structural edge
  };
  std::vector<DagEdge> dag;
  for (int e : part_edges) {
    int u = local[sedges[static_cast<std::size_t>(e)].first];
    int v = local[sedges[static_cast<std::size_t>(e)].second];
    if (order_of[static_cast<std::size_t>(u)] < order_of[static_cast<std::size_t>(v)]) {
      dag.push_back({u, v, e});
    } else {
      dag.push_back({v, u, e});
    }
  }
  std::vector<std::vector<int>> preds(static_cast<std::size_t>(k)), succs(static_cast<std::size_t>(k));
  for (const auto& d : dag) {
    succs[static_cast<std::size_t>(d.u)].push_back(d.v);
    preds[static_cast<std::size_t>(d.v)].push_back(d.u);
  }
  // --- 2. Ranks: the longest path, then each block pulled toward the side it has more lines to.
  std::vector<int> rank(static_cast<std::size_t>(k), 0);
  std::vector<int> topo;
  {
    std::vector<int> indeg(static_cast<std::size_t>(k), 0);
    for (const auto& d : dag) ++indeg[static_cast<std::size_t>(d.v)];
    for (int v = 0; v < k; ++v) {
      if (indeg[static_cast<std::size_t>(v)] == 0) topo.push_back(v);
    }
    for (std::size_t i = 0; i < topo.size(); ++i) {
      int u = topo[i];
      for (int v : succs[static_cast<std::size_t>(u)]) {
        rank[static_cast<std::size_t>(v)] = std::max(rank[static_cast<std::size_t>(v)], rank[static_cast<std::size_t>(u)] + 1);
        if (--indeg[static_cast<std::size_t>(v)] == 0) topo.push_back(v);
      }
    }
  }
  for (int iter = 0; iter < 32; ++iter) {
    bool changed = false;
    int max_rank = *std::max_element(rank.begin(), rank.end());
    for (std::size_t i = 0; i < topo.size(); ++i) {
      int v = topo[iter % 2 == 0 ? i : topo.size() - 1 - i];
      const auto& p = preds[static_cast<std::size_t>(v)];
      const auto& s = succs[static_cast<std::size_t>(v)];
      if (p.size() == s.size()) continue;
      int lo = 0;
      for (int u : p) lo = std::max(lo, rank[static_cast<std::size_t>(u)] + 1);
      int hi = max_rank;
      for (int w : s) hi = std::min(hi, rank[static_cast<std::size_t>(w)] - 1);
      if (lo > hi) continue;
      int target = p.size() > s.size() ? lo : hi;
      if (target != rank[static_cast<std::size_t>(v)]) {
        rank[static_cast<std::size_t>(v)] = target;
        changed = true;
      }
    }
    if (!changed) break;
  }
  {
    int lowest = *std::min_element(rank.begin(), rank.end());
    for (auto& r : rank) r -= lowest;
  }
  // --- 3. Vertices: the blocks, and dummies where lines pass over ranks. Long lines are concentrated:
  //        those of one block share a trunk of dummies (one way out of a block with many of them, one
  //        way into a block that many of them reach), and leave it at their own rank.
  std::vector<Vertex> vs;
  for (int i = 0; i < k; ++i) {
    const auto& n = g.nodes[static_cast<std::size_t>(nodes[static_cast<std::size_t>(i)])];
    vs.push_back({nodes[static_cast<std::size_t>(i)], n.w, n.h, rank[static_cast<std::size_t>(i)], 0});
  }
  Edges ledges;  // between adjacent ranks, each once
  std::set<std::pair<int, int>> ledge_set;
  struct Chain {
    int e;                  // structural edge
    std::vector<int> path;  // its vertices from its DAG tail to its head
  };
  std::vector<Chain> chains;
  std::vector<int> long_out(static_cast<std::size_t>(k), 0), long_in(static_cast<std::size_t>(k), 0);
  for (const auto& d : dag) {
    if (rank[static_cast<std::size_t>(d.v)] - rank[static_cast<std::size_t>(d.u)] < 2) continue;
    ++long_out[static_cast<std::size_t>(d.u)];
    ++long_in[static_cast<std::size_t>(d.v)];
  }
  std::map<int, std::map<int, int>> out_trunk, in_trunk;  // block → rank → dummy
  std::vector<std::vector<int>> trunks;                   // the dummies of each trunk (straightened together)
  std::map<std::pair<int, bool>, std::size_t> trunk_of;
  for (const auto& d : dag) {
    std::vector<int> path{d.u};
    int ru = rank[static_cast<std::size_t>(d.u)];
    int rv = rank[static_cast<std::size_t>(d.v)];
    if (rv - ru >= 2) {
      bool by_tail = long_out[static_cast<std::size_t>(d.u)] >= long_in[static_cast<std::size_t>(d.v)];
      int owner = by_tail ? d.u : d.v;
      auto& trunk = by_tail ? out_trunk[d.u] : in_trunk[d.v];
      auto [ti, made] = trunk_of.try_emplace({owner, by_tail}, trunks.size());
      if (made) trunks.emplace_back();
      for (int r = ru + 1; r < rv; ++r) {
        auto it = trunk.find(r);
        if (it == trunk.end()) {
          vs.push_back({-1, 0, 0, r, 0});
          it = trunk.emplace(r, static_cast<int>(vs.size()) - 1).first;
          trunks[ti->second].push_back(it->second);
        }
        path.push_back(it->second);
      }
    }
    path.push_back(d.v);
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      if (ledge_set.insert({path[i], path[i + 1]}).second) ledges.emplace_back(path[i], path[i + 1]);
    }
    chains.push_back({d.e, std::move(path)});
  }
  const auto nv = vs.size();
  int ranks = 0;
  for (const auto& v : vs) ranks = std::max(ranks, v.rank + 1);
  std::vector<std::vector<int>> up(nv), down(nv);  // neighbors on the rank before / after
  for (const auto& [a, b] : ledges) {
    down[static_cast<std::size_t>(a)].push_back(b);
    up[static_cast<std::size_t>(b)].push_back(a);
  }
  // --- 4. Order inside the ranks: a depth-first start, then median sweeps and swaps of neighbors.
  std::vector<std::vector<int>> layer(static_cast<std::size_t>(ranks));
  {
    std::vector<bool> seen(nv, false);
    std::function<void(int)> visit = [&](int v) {
      seen[static_cast<std::size_t>(v)] = true;
      layer[static_cast<std::size_t>(vs[static_cast<std::size_t>(v)].rank)].push_back(v);
      for (int w : down[static_cast<std::size_t>(v)]) {
        if (!seen[static_cast<std::size_t>(w)]) visit(w);
      }
    };
    for (int r = 0; r < ranks; ++r) {
      for (std::size_t v = 0; v < nv; ++v) {
        if (!seen[v] && vs[v].rank == r && up[v].empty()) visit(static_cast<int>(v));
      }
    }
    for (std::size_t v = 0; v < nv; ++v) {
      if (!seen[v]) visit(static_cast<int>(v));
    }
  }
  std::vector<int> pos(nv, 0);
  auto renumber = [&] {
    for (auto& l : layer) {
      for (std::size_t i = 0; i < l.size(); ++i) pos[static_cast<std::size_t>(l[i])] = static_cast<int>(i);
    }
  };
  renumber();
  auto layer_crossings = [&](int r) {  // between rank r and r + 1
    std::vector<std::pair<int, int>> e;
    for (int v : layer[static_cast<std::size_t>(r)]) {
      for (int w : down[static_cast<std::size_t>(v)]) e.emplace_back(pos[static_cast<std::size_t>(v)], pos[static_cast<std::size_t>(w)]);
    }
    return crossings(std::move(e), static_cast<int>(layer[static_cast<std::size_t>(r + 1)].size()));
  };
  auto total_crossings = [&] {
    long c = 0;
    for (int r = 0; r + 1 < ranks; ++r) c += layer_crossings(r);
    return c;
  };
  // The crossings of the lines of a and b (a before b) with one neighbor rank.
  auto pair_crossings = [&](int a, int b, const std::vector<std::vector<int>>& nb) {
    long c = 0;
    for (int x : nb[static_cast<std::size_t>(a)]) {
      for (int y : nb[static_cast<std::size_t>(b)]) c += pos[static_cast<std::size_t>(x)] > pos[static_cast<std::size_t>(y)] ? 1 : 0;
    }
    return c;
  };
  auto median = [&](int v, const std::vector<std::vector<int>>& nb) -> double {
    std::vector<int> p;
    for (int u : nb[static_cast<std::size_t>(v)]) p.push_back(pos[static_cast<std::size_t>(u)]);
    if (p.empty()) return -1;
    std::sort(p.begin(), p.end());
    std::size_t m = p.size() / 2;
    if (p.size() % 2 == 1) return p[m];
    if (p.size() == 2) return (p[0] + p[1]) / 2.0;
    double l = p[m - 1] - p.front();
    double r = p.back() - p[m];
    return l + r == 0 ? (p[m - 1] + p[m]) / 2.0 : (p[m - 1] * r + p[m] * l) / (l + r);
  };
  auto best = layer;
  long best_c = total_crossings();
  for (int iter = 0; iter < 24 && best_c > 0; ++iter) {
    bool downward = iter % 2 == 0;
    for (int step = 1; step < ranks; ++step) {
      int r = downward ? step : ranks - 1 - step;
      const auto& nb = downward ? up : down;
      auto& l = layer[static_cast<std::size_t>(r)];
      std::vector<std::pair<double, int>> keyed;
      for (int v : l) {
        double m = median(v, nb);
        keyed.emplace_back(m < 0 ? pos[static_cast<std::size_t>(v)] : m, v);
      }
      std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      for (std::size_t i = 0; i < l.size(); ++i) l[i] = keyed[i].second;
      renumber();
    }
    for (int pass = 0; pass < 4; ++pass) {  // transpose: swap neighbors while it removes crossings
      bool improved = false;
      for (auto& l : layer) {
        for (std::size_t i = 0; i + 1 < l.size(); ++i) {
          int a = l[i];
          int b = l[i + 1];
          long now = pair_crossings(a, b, up) + pair_crossings(a, b, down);
          long swapped = pair_crossings(b, a, up) + pair_crossings(b, a, down);
          if (swapped < now) {
            std::swap(l[i], l[i + 1]);
            pos[static_cast<std::size_t>(a)] = static_cast<int>(i + 1);
            pos[static_cast<std::size_t>(b)] = static_cast<int>(i);
            improved = true;
          }
        }
      }
      if (!improved) break;
    }
    long c = total_crossings();
    if (c < best_c) {
      best_c = c;
      best = layer;
    }
  }
  layer = best;
  renumber();
  // --- 5. Heights. Inside a rank each vertex wants the weighted mean of its neighbors' line points
  //        (a block's lines attach at its anchor; lines between dummies weigh most, so long lines run
  //        straight), and the order with its gaps is kept: an isotonic regression per rank.
  std::vector<double> anchor(nv, 0.0);  // from a vertex's center to where its lines attach
  for (std::size_t i = 0; i < nv; ++i) {
    if (vs[i].node < 0) continue;
    double a = g.nodes[static_cast<std::size_t>(vs[i].node)].anchor;
    anchor[i] = (a >= 0 && a <= vs[i].h ? a : vs[i].h / 2) - vs[i].h / 2;
  }
  auto port = [&](int v) { return vs[static_cast<std::size_t>(v)].y + anchor[static_cast<std::size_t>(v)]; };
  auto sep = [&](int a, int b) {
    bool da = vs[static_cast<std::size_t>(a)].node < 0;
    bool db = vs[static_cast<std::size_t>(b)].node < 0;
    if (da && db) return 30.0;  // parallel lines far enough apart to stay apart (thick bundles too)
    if (da || db) return std::max(30.0, g.node_sep * 0.8);
    return g.node_sep;
  };
  auto weight = [&](int a, int b) {
    int d = (vs[static_cast<std::size_t>(a)].node < 0 ? 1 : 0) + (vs[static_cast<std::size_t>(b)].node < 0 ? 1 : 0);
    return d == 2 ? 8.0 : d == 1 ? 2.0 : 1.0;
  };
  for (auto& l : layer) {
    double y = 0;
    for (std::size_t i = 0; i < l.size(); ++i) {
      auto& v = vs[static_cast<std::size_t>(l[i])];
      if (i > 0) y += vs[static_cast<std::size_t>(l[i - 1])].h / 2 + v.h / 2 + sep(l[i - 1], l[i]);
      v.y = y;
    }
  }
  std::vector<double> fixed(nv, std::nan(""));  // the last passes: where a long line's dummies must be
  auto place = [&](int r, bool use_up, bool use_down) {
    const auto& l = layer[static_cast<std::size_t>(r)];
    if (l.empty()) return;
    std::vector<double> d(l.size()), w(l.size()), off(l.size(), 0.0);
    for (std::size_t i = 0; i < l.size(); ++i) {
      int v = l[i];
      if (i > 0) off[i] = off[i - 1] + vs[static_cast<std::size_t>(l[i - 1])].h / 2 + vs[static_cast<std::size_t>(v)].h / 2 + sep(l[i - 1], v);
      if (!std::isnan(fixed[static_cast<std::size_t>(v)])) {
        d[i] = fixed[static_cast<std::size_t>(v)] - off[i];
        w[i] = 1e6;  // the blocks around give way
        continue;
      }
      double sw = 0;
      double swy = 0;
      auto add = [&](const std::vector<int>& nb) {
        for (int u : nb) {
          double ew = weight(v, u);
          sw += ew;
          swy += ew * (port(u) - anchor[static_cast<std::size_t>(v)]);
        }
      };
      if (use_up) add(up[static_cast<std::size_t>(v)]);
      if (use_down) add(down[static_cast<std::size_t>(v)]);
      if (sw == 0) {  // nothing to follow: stays where it is
        sw = 0.05;
        swy = 0.05 * vs[static_cast<std::size_t>(v)].y;
      }
      d[i] = swy / sw - off[i];
      w[i] = sw;
    }
    auto z = isotonic(d, w);
    for (std::size_t i = 0; i < l.size(); ++i) vs[static_cast<std::size_t>(l[i])].y = z[i] + off[i];
  };
  for (int pass = 0; pass < 24; ++pass) {
    bool both = pass >= 16;
    bool downward = pass % 2 == 0;
    for (int step = 0; step < ranks; ++step) {
      int r = downward ? step : ranks - 1 - step;
      place(r, both || downward, both || !downward);
    }
  }
  // Long lines straight: the dummies of a line at one height (their mean), the blocks placed again
  // around them.
  for (int round = 0; round < 3; ++round) {
    for (const auto& t : trunks) {
      if (t.empty()) continue;
      double mean = 0;
      for (int id : t) mean += vs[static_cast<std::size_t>(id)].y;
      mean /= static_cast<double>(t.size());
      for (int id : t) fixed[static_cast<std::size_t>(id)] = mean;
    }
    for (int r = 0; r < ranks; ++r) place(r, true, true);
  }
  // --- 6. Orthogonal routes. Every piece between two ranks that changes height turns at its own track
  //        in the gap (pieces a trunk shares turn once); tracks are ordered pair by pair to cross least,
  //        a track is shared by pieces whose turns do not overlap, and a gap is as wide as its tracks need.
  struct Turn {
    std::size_t ledge;
    double from, to;  // heights on the left and on the right of the gap
    int track = -1;   // -1: level, no turn
  };
  std::vector<std::vector<Turn>> gaps(static_cast<std::size_t>(std::max(ranks - 1, 0)));
  std::map<std::pair<int, int>, std::pair<std::size_t, std::size_t>> turn_of;  // ledge → gap, index
  for (std::size_t li = 0; li < ledges.size(); ++li) {
    auto [a, b] = ledges[li];
    auto gi = static_cast<std::size_t>(vs[static_cast<std::size_t>(a)].rank);
    turn_of[{a, b}] = {gi, gaps[gi].size()};
    gaps[gi].push_back({li, port(a), port(b), -1});
  }
  constexpr double kTrack = 20;    // between tracks
  constexpr double kMarginX = 34;  // from a column to its first track
  std::vector<int> tracks(gaps.size(), 0);
  for (std::size_t gi = 0; gi < gaps.size(); ++gi) {
    auto& turns = gaps[gi];
    std::vector<std::size_t> bent;
    for (std::size_t i = 0; i < turns.size(); ++i) {
      if (std::fabs(turns[i].from - turns[i].to) > 0.5) bent.push_back(i);
    }
    auto inside = [](double y, double a, double b) { return y > std::min(a, b) + 0.5 && y < std::max(a, b) - 0.5; };
    // Crossings when turn a lies left of turn b: b's way in passes a's turn, a's way out passes b's.
    auto cost = [&](const Turn& a, const Turn& b) { return (inside(b.from, a.from, a.to) ? 1 : 0) + (inside(a.to, b.from, b.to) ? 1 : 0); };
    std::vector<int> before(bent.size(), 0);  // how many turns want to be left of this one
    for (std::size_t i = 0; i < bent.size(); ++i) {
      for (std::size_t j = i + 1; j < bent.size(); ++j) {
        const auto& a = turns[bent[i]];
        const auto& b = turns[bent[j]];
        int ab = cost(a, b);
        int ba = cost(b, a);
        if (ab < ba) {
          ++before[j];
        } else if (ba < ab) {
          ++before[i];
        } else if (std::min(a.from, a.to) > std::min(b.from, b.to)) {  // a tie: the lower one later
          ++before[i];
        } else {
          ++before[j];
        }
      }
    }
    std::vector<std::size_t> order(bent.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return before[a] < before[b]; });
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
      auto& t = turns[bent[order[oi]]];
      int track = 0;
      for (std::size_t oj = 0; oj < oi; ++oj) {
        const auto& u = turns[bent[order[oj]]];
        bool overlap = std::min(t.from, t.to) < std::max(u.from, u.to) + 16 && std::min(u.from, u.to) < std::max(t.from, t.to) + 16;
        if (overlap) track = std::max(track, u.track + 1);
      }
      t.track = track;
      tracks[gi] = std::max(tracks[gi], track + 1);
    }
  }
  std::vector<double> rank_w(static_cast<std::size_t>(ranks), 10.0), rank_x(static_cast<std::size_t>(ranks), 0.0);
  std::vector<double> gap_w(gaps.size(), g.rank_sep);
  for (const auto& v : vs) rank_w[static_cast<std::size_t>(v.rank)] = std::max(rank_w[static_cast<std::size_t>(v.rank)], v.w);
  for (std::size_t gi = 0; gi < gaps.size(); ++gi) {
    gap_w[gi] = std::max(g.rank_sep, 2 * kMarginX + std::max(0, tracks[gi] - 1) * kTrack);
    rank_x[gi + 1] = rank_x[gi] + rank_w[gi] + gap_w[gi];
  }
  double top = 1e18;
  for (const auto& v : vs) top = std::min(top, v.y - v.h / 2);
  for (const auto& v : vs) {
    if (v.node < 0) continue;
    double x = rank_x[static_cast<std::size_t>(v.rank)] + (rank_w[static_cast<std::size_t>(v.rank)] - v.w) / 2;
    double y = v.y - v.h / 2 - top;
    out.pos[v.node] = Pos{x, y};
    out.width = std::max(out.width, x + v.w);
    out.height = std::max(out.height, y + v.h);
  }
  auto turn_x = [&](std::size_t gi, const Turn& t) {
    double gap_left = rank_x[gi] + rank_w[gi];
    double span = std::max(0, tracks[gi] - 1) * kTrack;
    return t.track < 0 ? gap_left + gap_w[gi] / 2 : gap_left + (gap_w[gi] - span) / 2 + t.track * kTrack;
  };
  for (const auto& c : chains) {
    std::vector<Point> pts;
    auto add = [&](Point pt) {
      if (!pts.empty() && std::fabs(pts.back().x - pt.x) < 0.01 && std::fabs(pts.back().y - pt.y) < 0.01) return;
      pts.push_back(pt);
      out.height = std::max(out.height, pt.y);
    };
    for (std::size_t i = 0; i + 1 < c.path.size(); ++i) {
      auto [gi, ti] = turn_of[{c.path[i], c.path[i + 1]}];
      const auto& t = gaps[gi][ti];
      double x = turn_x(gi, t);
      add({x, t.from - top});
      add({x, t.to - top});
    }
    out.chains[c.e] = std::move(pts);
  }
  return out;
}

}  // namespace

kk::Result<Layout> SugiyamaEngine::layout(const Graph& g) {
  Layout out;
  out.engine = name();
  const int n = static_cast<int>(g.nodes.size());
  if (n == 0) return out;
  std::map<std::string, int> idx;
  for (int i = 0; i < n; ++i) idx.emplace(g.nodes[static_cast<std::size_t>(i)].id, i);
  // The lines, and one structural edge per pair of blocks: a pair linked both ways is one edge in the
  // order of the blocks (notes that link back and forth read in their own order).
  struct Line {
    int u, v;
    std::string kind;
  };
  std::vector<Line> lines;
  std::map<std::pair<int, int>, std::pair<bool, bool>> pairs;  // (lo, hi) → lo→hi, hi→lo
  {
    std::set<std::tuple<int, int, std::string>> seen;
    for (const auto& e : g.edges) {
      auto a = idx.find(e.from);
      auto b = idx.find(e.to);
      if (a == idx.end() || b == idx.end() || a->second == b->second) continue;
      if (!seen.insert({a->second, b->second, e.kind}).second) continue;
      lines.push_back({a->second, b->second, e.kind});
      auto key = std::minmax(a->second, b->second);
      auto& dir = pairs[{key.first, key.second}];
      bool forward = a->second < b->second;
      (forward ? dir.first : dir.second) = true;
    }
  }
  std::vector<std::pair<int, int>> sedges;
  std::map<std::pair<int, int>, int> sedge_of;  // (lo, hi) → structural edge
  for (const auto& [p, dir] : pairs) {
    auto [lo, hi] = p;
    std::pair<int, int> e;
    if (dir.first && dir.second) {
      e = {lo, hi};
    } else {
      e = dir.first ? std::pair{lo, hi} : std::pair{hi, lo};
    }
    sedge_of[p] = static_cast<int>(sedges.size());
    sedges.push_back(e);
  }
  // Connected parts (union-find), each laid out alone; a part in the order of its first block.
  std::vector<int> parent(static_cast<std::size_t>(n));
  std::iota(parent.begin(), parent.end(), 0);
  std::function<int(int)> find = [&](int x) { return parent[static_cast<std::size_t>(x)] == x ? x : parent[static_cast<std::size_t>(x)] = find(parent[static_cast<std::size_t>(x)]); };
  for (const auto& [u, v] : sedges) parent[static_cast<std::size_t>(find(u))] = find(v);
  std::map<int, int> part_of_root;
  std::vector<std::vector<int>> part_nodes;
  std::vector<std::vector<int>> part_edges;
  for (int i = 0; i < n; ++i) {
    auto [it, made] = part_of_root.try_emplace(find(i), static_cast<int>(part_nodes.size()));
    if (made) {
      part_nodes.emplace_back();
      part_edges.emplace_back();
    }
    part_nodes[static_cast<std::size_t>(it->second)].push_back(i);
  }
  for (int e = 0; e < static_cast<int>(sedges.size()); ++e) {
    part_edges[static_cast<std::size_t>(part_of_root[find(sedges[static_cast<std::size_t>(e)].first)])].push_back(e);
  }
  std::vector<Part> parts;
  for (std::size_t p = 0; p < part_nodes.size(); ++p) parts.push_back(layout_part(g, part_nodes[p], sedges, part_edges[p]));
  // Packing: linked parts on shelves (the biggest first), single blocks in rows under them, all within
  // a width that keeps the picture about 16:10.
  std::vector<std::size_t> multi, single;
  double area = 0;
  double widest = 0;
  for (std::size_t p = 0; p < parts.size(); ++p) {
    (part_nodes[p].size() > 1 ? multi : single).push_back(p);
    area += (parts[p].width + g.rank_sep) * (parts[p].height + g.node_sep);
    widest = std::max(widest, parts[p].width);
  }
  std::stable_sort(multi.begin(), multi.end(), [&](std::size_t a, std::size_t b) { return part_nodes[a].size() > part_nodes[b].size(); });
  const double limit = std::max(widest, std::sqrt(area * 1.6));
  constexpr double kMargin = 20;
  std::vector<Point> offset(parts.size());
  double x = 0;
  double y = 0;
  double shelf = 0;
  auto put = [&](std::size_t p, double gap_x, double gap_y) {
    if (x > 0 && x + parts[p].width > limit) {
      x = 0;
      y += shelf + gap_y;
      shelf = 0;
    }
    offset[p] = {kMargin + x, kMargin + y};
    x += parts[p].width + gap_x;
    shelf = std::max(shelf, parts[p].height);
  };
  for (auto p : multi) put(p, g.rank_sep, g.node_sep * 2);
  if (!multi.empty() && !single.empty()) {
    x = 0;
    y += shelf + g.node_sep * 2;
    shelf = 0;
  }
  for (auto p : single) put(p, g.node_sep * 1.5, g.node_sep);
  for (std::size_t p = 0; p < parts.size(); ++p) {
    for (const auto& [node, pp] : parts[p].pos) {
      Pos at{pp.x + offset[p].x, pp.y + offset[p].y};
      out.pos[g.nodes[static_cast<std::size_t>(node)].id] = at;
      out.width = std::max(out.width, at.x + g.nodes[static_cast<std::size_t>(node)].w + kMargin);
      out.height = std::max(out.height, at.y + g.nodes[static_cast<std::size_t>(node)].h + kMargin);
    }
  }
  // Routes of the lines: their structural edge's points, turned round for a line that goes the other
  // way; several lines of one pair of blocks lie side by side.
  std::map<std::pair<int, int>, std::vector<std::size_t>> by_pair;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    auto key = std::minmax(lines[i].u, lines[i].v);
    by_pair[{key.first, key.second}].push_back(i);
  }
  out.routes.resize(lines.size());
  for (const auto& [p, members] : by_pair) {
    int e = sedge_of[p];
    std::size_t part = static_cast<std::size_t>(part_of_root[find(p.first)]);
    const auto& chain = parts[part].chains[e];
    // The chain runs from the tail of the edge in the layout's order (its DAG direction).
    int dag_tail = sedges[static_cast<std::size_t>(e)].first;
    if (!chain.empty()) {
      // A reversed edge's chain was made from its other end: find which end is on the left.
      const auto& pu = out.pos[g.nodes[static_cast<std::size_t>(sedges[static_cast<std::size_t>(e)].first)].id];
      const auto& pv = out.pos[g.nodes[static_cast<std::size_t>(sedges[static_cast<std::size_t>(e)].second)].id];
      dag_tail = pu.x <= pv.x ? sedges[static_cast<std::size_t>(e)].first : sedges[static_cast<std::size_t>(e)].second;
    }
    for (std::size_t m = 0; m < members.size(); ++m) {
      const auto& line = lines[members[m]];
      auto& r = out.routes[members[m]];
      r.from = g.nodes[static_cast<std::size_t>(line.u)].id;
      r.to = g.nodes[static_cast<std::size_t>(line.v)].id;
      r.kind = line.kind;
      double shift = (static_cast<double>(m) - static_cast<double>(members.size() - 1) / 2.0) * 10.0;
      for (const auto& pt : chain) r.points.push_back({pt.x + offset[part].x + shift, pt.y + offset[part].y + shift});
      if (line.u != dag_tail) std::reverse(r.points.begin(), r.points.end());
    }
  }
  return out;
}

}  // namespace kk::layout
