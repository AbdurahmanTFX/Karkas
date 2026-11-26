#include "graph.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace kk::hsgm::detail {

namespace {

std::vector<std::string> code_ids(const Input& in) {
  std::vector<std::string> ids;
  for (const auto& b : in.snap.blocks) {
    if (is_code_block(b)) ids.push_back(b.id);
  }
  return ids;
}

}  // namespace

Graph make_graph(std::vector<std::string> nodes, const std::vector<std::pair<std::string, std::string>>& edges) {
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  Graph g;
  g.nodes = std::move(nodes);
  for (std::size_t i = 0; i < g.nodes.size(); ++i) g.at[g.nodes[i]] = static_cast<int>(i);
  std::vector<std::set<int>> out(g.nodes.size());
  for (const auto& [a, b] : edges) {
    auto ia = g.at.find(a);
    auto ib = g.at.find(b);
    if (ia == g.at.end() || ib == g.at.end() || ia->second == ib->second) continue;
    out[static_cast<std::size_t>(ia->second)].insert(ib->second);
  }
  for (const auto& s : out) g.out.emplace_back(s.begin(), s.end());
  return g;
}

Graph drawn_graph(const Input& in) {
  std::vector<std::pair<std::string, std::string>> edges;
  for (const auto& l : in.snap.links) {
    if (l.kind == "data" || l.kind == "event" || l.kind == "call" || l.kind == "state") edges.emplace_back(l.src_block, l.dst_block);
  }
  return make_graph(code_ids(in), edges);
}

Graph dependency_graph(const Input& in) {
  std::vector<std::pair<std::string, std::string>> edges;
  for (const auto& l : in.snap.links) {
    if (l.kind == "data" || l.kind == "event") {
      edges.emplace_back(l.dst_block, l.src_block);
    } else if (l.kind == "call" || l.kind == "use") {
      edges.emplace_back(l.src_block, l.dst_block);
    } else if (l.kind == "state") {
      edges.emplace_back(l.src_block, l.dst_block);
      edges.emplace_back(l.dst_block, l.src_block);
    }
  }
  return make_graph(code_ids(in), edges);
}

std::vector<std::vector<char>> reach(const Graph& g, bool reflexive) {
  const std::size_t n = g.nodes.size();
  std::vector<std::vector<char>> r(n, std::vector<char>(n, 0));
  for (std::size_t s = 0; s < n; ++s) {
    std::vector<int> stack(g.out[s].begin(), g.out[s].end());
    for (int v : stack) r[s][static_cast<std::size_t>(v)] = 1;
    while (!stack.empty()) {
      int x = stack.back();
      stack.pop_back();
      for (int y : g.out[static_cast<std::size_t>(x)]) {
        auto& seen = r[s][static_cast<std::size_t>(y)];
        if (seen == 0) {
          seen = 1;
          stack.push_back(y);
        }
      }
    }
    if (reflexive) r[s][s] = 1;
  }
  return r;
}

std::vector<std::vector<int>> sccs(const Graph& g) {
  // Tarjan, iterative.
  const int n = static_cast<int>(g.nodes.size());
  std::vector<int> index(static_cast<std::size_t>(n), -1), low(static_cast<std::size_t>(n), 0);
  std::vector<char> on_stack(static_cast<std::size_t>(n), 0);
  std::vector<int> stack;
  std::vector<std::vector<int>> out;
  int next = 0;
  auto u = [](int v) { return static_cast<std::size_t>(v); };
  for (int root = 0; root < n; ++root) {
    if (index[u(root)] != -1) continue;
    std::vector<std::pair<int, std::size_t>> call{{root, 0}};  // node, next edge
    index[u(root)] = low[u(root)] = next++;
    stack.push_back(root);
    on_stack[u(root)] = 1;
    while (!call.empty()) {
      auto& [v, e] = call.back();
      const auto& edges = g.out[u(v)];
      if (e < edges.size()) {
        int w = edges[e++];
        if (index[u(w)] == -1) {
          index[u(w)] = low[u(w)] = next++;
          stack.push_back(w);
          on_stack[u(w)] = 1;
          call.emplace_back(w, 0);
        } else if (on_stack[u(w)] != 0) {
          low[u(v)] = std::min(low[u(v)], index[u(w)]);
        }
        continue;
      }
      if (low[u(v)] == index[u(v)]) {
        std::vector<int> comp;
        int w = -1;
        do {
          w = stack.back();
          stack.pop_back();
          on_stack[u(w)] = 0;
          comp.push_back(w);
        } while (w != v);
        std::sort(comp.begin(), comp.end());
        out.push_back(std::move(comp));
      }
      int done = v;
      call.pop_back();
      if (!call.empty()) {
        int parent = call.back().first;
        low[u(parent)] = std::min(low[u(parent)], low[u(done)]);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace kk::hsgm::detail
