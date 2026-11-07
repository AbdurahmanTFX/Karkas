#include <algorithm>
#include <map>
#include <numeric>
#include <set>

#include "kk/layout/layout.hpp"

namespace kk::layout {

namespace {

struct V {
  double w = 0, h = 0;
  bool dummy = false;
  int rank = 0;
  double y = 0;  // center
};

long crossings(const std::vector<int>& top, const std::vector<int>& bottom, const std::vector<std::pair<int, int>>& edges,
               const std::vector<int>& pos) {
  std::vector<std::pair<int, int>> e;
  std::set<int> t(top.begin(), top.end());
  std::set<int> b(bottom.begin(), bottom.end());
  for (const auto& [u, v] : edges) {
    if (t.contains(u) && b.contains(v)) e.emplace_back(pos[static_cast<std::size_t>(u)], pos[static_cast<std::size_t>(v)]);
  }
  long c = 0;
  for (std::size_t i = 0; i < e.size(); ++i) {
    for (std::size_t j = i + 1; j < e.size(); ++j) {
      if ((e[i].first - e[j].first) * (e[i].second - e[j].second) < 0) ++c;
    }
  }
  return c;
}

}  // namespace

kk::Result<Layout> SugiyamaEngine::layout(const Graph& g) {
  Layout out;
  out.engine = name();
  const int n = static_cast<int>(g.nodes.size());
  if (n == 0) return out;
  std::map<std::string, int> idx;
  for (int i = 0; i < n; ++i) idx[g.nodes[static_cast<std::size_t>(i)].id] = i;
  std::set<std::pair<int, int>> eset;
  for (const auto& e : g.edges) {
    auto a = idx.find(e.from);
    auto b = idx.find(e.to);
    if (a == idx.end() || b == idx.end() || a->second == b->second) continue;
    eset.insert({a->second, b->second});
  }
  // 1. Cycle removal: reverse DFS back edges.
  std::vector<std::vector<int>> adj(static_cast<std::size_t>(n));
  for (const auto& [u, v] : eset) adj[static_cast<std::size_t>(u)].push_back(v);
  std::vector<int> color(static_cast<std::size_t>(n), 0);
  std::set<std::pair<int, int>> dag;
  std::function<void(int)> dfs = [&](int u) {
    color[static_cast<std::size_t>(u)] = 1;
    for (int v : adj[static_cast<std::size_t>(u)]) {
      if (color[static_cast<std::size_t>(v)] == 1) {
        dag.insert({v, u});
      } else {
        dag.insert({u, v});
        if (color[static_cast<std::size_t>(v)] == 0) dfs(v);
      }
    }
    color[static_cast<std::size_t>(u)] = 2;
  };
  for (int i = 0; i < n; ++i) {
    if (color[static_cast<std::size_t>(i)] == 0) dfs(i);
  }
  // 2. Ranks by longest path.
  std::vector<V> vs(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    vs[static_cast<std::size_t>(i)].w = g.nodes[static_cast<std::size_t>(i)].w;
    vs[static_cast<std::size_t>(i)].h = g.nodes[static_cast<std::size_t>(i)].h;
  }
  std::vector<int> indeg(static_cast<std::size_t>(n), 0);
  std::vector<std::vector<int>> succ(static_cast<std::size_t>(n));
  for (const auto& [u, v] : dag) {
    succ[static_cast<std::size_t>(u)].push_back(v);
    ++indeg[static_cast<std::size_t>(v)];
  }
  std::vector<int> queue;
  for (int i = 0; i < n; ++i) {
    if (indeg[static_cast<std::size_t>(i)] == 0) queue.push_back(i);
  }
  for (std::size_t qi = 0; qi < queue.size(); ++qi) {
    int u = queue[qi];
    for (int v : succ[static_cast<std::size_t>(u)]) {
      vs[static_cast<std::size_t>(v)].rank = std::max(vs[static_cast<std::size_t>(v)].rank, vs[static_cast<std::size_t>(u)].rank + 1);
      if (--indeg[static_cast<std::size_t>(v)] == 0) queue.push_back(v);
    }
  }
  // 3. Dummy nodes for edges spanning several ranks.
  std::vector<std::pair<int, int>> edges;
  for (const auto& [u, v] : dag) {
    int prev = u;
    for (int r = vs[static_cast<std::size_t>(u)].rank + 1; r < vs[static_cast<std::size_t>(v)].rank; ++r) {
      V d;
      d.dummy = true;
      d.rank = r;
      d.w = 10;
      d.h = 10;
      vs.push_back(d);
      int id = static_cast<int>(vs.size()) - 1;
      edges.emplace_back(prev, id);
      prev = id;
    }
    edges.emplace_back(prev, v);
  }
  int ranks = 0;
  for (const auto& v : vs) ranks = std::max(ranks, v.rank + 1);
  std::vector<std::vector<int>> layer(static_cast<std::size_t>(ranks));
  for (int i = 0; i < static_cast<int>(vs.size()); ++i) layer[static_cast<std::size_t>(vs[static_cast<std::size_t>(i)].rank)].push_back(i);
  std::vector<std::vector<int>> preds(vs.size()), succs(vs.size());
  for (const auto& [u, v] : edges) {
    preds[static_cast<std::size_t>(v)].push_back(u);
    succs[static_cast<std::size_t>(u)].push_back(v);
  }
  std::vector<int> pos(vs.size(), 0);
  auto renumber = [&] {
    for (auto& l : layer) {
      for (std::size_t i = 0; i < l.size(); ++i) pos[static_cast<std::size_t>(l[i])] = static_cast<int>(i);
    }
  };
  renumber();
  auto total_crossings = [&] {
    long c = 0;
    for (int r = 0; r + 1 < ranks; ++r) c += crossings(layer[static_cast<std::size_t>(r)], layer[static_cast<std::size_t>(r + 1)], edges, pos);
    return c;
  };
  // 4. Crossing reduction (barycenter sweeps), keep the best order.
  auto best = layer;
  long best_c = total_crossings();
  for (int iter = 0; iter < 12 && best_c > 0; ++iter) {
    bool down = iter % 2 == 0;
    for (int k = 1; k < ranks; ++k) {
      int r = down ? k : ranks - 1 - k;
      auto& l = layer[static_cast<std::size_t>(r)];
      std::vector<std::pair<double, int>> keyed;
      for (int v : l) {
        const auto& nb = down ? preds[static_cast<std::size_t>(v)] : succs[static_cast<std::size_t>(v)];
        double key = pos[static_cast<std::size_t>(v)];
        if (!nb.empty()) {
          double s = 0;
          for (int u : nb) s += pos[static_cast<std::size_t>(u)];
          key = s / static_cast<double>(nb.size());
        }
        keyed.emplace_back(key, v);
      }
      std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      for (std::size_t i = 0; i < l.size(); ++i) l[i] = keyed[i].second;
      renumber();
    }
    long c = total_crossings();
    if (c < best_c) {
      best_c = c;
      best = layer;
    }
  }
  layer = best;
  renumber();
  // 5. Coordinates: ranks left to right, nodes stacked inside a rank, then pulled
  //    towards the barycenter of their neighbors without overlapping.
  std::vector<double> rank_x(static_cast<std::size_t>(ranks), 0), rank_w(static_cast<std::size_t>(ranks), 0);
  for (const auto& v : vs) rank_w[static_cast<std::size_t>(v.rank)] = std::max(rank_w[static_cast<std::size_t>(v.rank)], v.w);
  for (int r = 1; r < ranks; ++r) {
    rank_x[static_cast<std::size_t>(r)] = rank_x[static_cast<std::size_t>(r - 1)] + rank_w[static_cast<std::size_t>(r - 1)] + g.rank_sep;
  }
  for (auto& l : layer) {
    double y = 0;
    for (int v : l) {
      auto& x = vs[static_cast<std::size_t>(v)];
      x.y = y + x.h / 2;
      y += x.h + g.node_sep;
    }
  }
  for (int pass = 0; pass < 8; ++pass) {
    bool down = pass % 2 == 0;
    for (int k = 0; k < ranks; ++k) {
      int r = down ? k : ranks - 1 - k;
      auto& l = layer[static_cast<std::size_t>(r)];
      std::vector<double> want(l.size());
      for (std::size_t i = 0; i < l.size(); ++i) {
        int v = l[i];
        std::vector<int> nb = preds[static_cast<std::size_t>(v)];
        nb.insert(nb.end(), succs[static_cast<std::size_t>(v)].begin(), succs[static_cast<std::size_t>(v)].end());
        double w = vs[static_cast<std::size_t>(v)].y;
        if (!nb.empty()) {
          double s = 0;
          for (int u : nb) s += vs[static_cast<std::size_t>(u)].y;
          w = s / static_cast<double>(nb.size());
        }
        want[i] = w;
      }
      // Forward: no overlap; then shift the rank to keep its mean where it wants to be.
      std::vector<double> y(l.size());
      for (std::size_t i = 0; i < l.size(); ++i) {
        const auto& a = vs[static_cast<std::size_t>(l[i])];
        y[i] = want[i];
        if (i > 0) {
          const auto& b = vs[static_cast<std::size_t>(l[i - 1])];
          y[i] = std::max(y[i], y[i - 1] + (a.h + b.h) / 2 + g.node_sep);
        }
      }
      double shift = 0;
      for (std::size_t i = 0; i < l.size(); ++i) shift += want[i] - y[i];
      shift /= static_cast<double>(std::max<std::size_t>(l.size(), 1));
      for (std::size_t i = 0; i < l.size(); ++i) vs[static_cast<std::size_t>(l[i])].y = y[i] + shift;
    }
  }
  double min_top = 1e18;
  for (const auto& v : vs) min_top = std::min(min_top, v.y - v.h / 2);
  constexpr double kMargin = 20;
  for (int i = 0; i < n; ++i) {
    const auto& v = vs[static_cast<std::size_t>(i)];
    double cx = rank_x[static_cast<std::size_t>(v.rank)] + rank_w[static_cast<std::size_t>(v.rank)] / 2;
    Pos p{kMargin + cx - v.w / 2, kMargin + v.y - v.h / 2 - min_top};
    out.pos[g.nodes[static_cast<std::size_t>(i)].id] = p;
    out.width = std::max(out.width, p.x + v.w + kMargin);
    out.height = std::max(out.height, p.y + v.h + kMargin);
  }
  return out;
}

}  // namespace kk::layout
