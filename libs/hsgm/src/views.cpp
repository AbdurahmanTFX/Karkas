#include "kk/hsgm/views.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <set>

#include "graph.hpp"
#include "kk/core/text.hpp"
#include "text.hpp"

namespace kk::hsgm {

Views views(const Input& in) {
  Views out;
  const auto leaf = detail::drawn_graph(in);
  const std::size_t n = leaf.nodes.size();
  int max_depth = 0;
  for (const auto& id : leaf.nodes) {
    if (const auto* b = in.snap.block(id)) max_depth = std::max(max_depth, b->depth);
  }
  const auto leaf_reach = detail::reach(leaf, false);
  const auto leaf_sccs = detail::sccs(leaf);

  for (int level = 0; level <= max_depth - 2; ++level) {
    std::vector<std::string> up(n);  // a_level of every block
    for (std::size_t i = 0; i < n; ++i) up[i] = ancestor_on(leaf.nodes[i], level);
    std::vector<std::pair<std::string, std::string>> edges;
    for (std::size_t i = 0; i < n; ++i) {
      for (int j : leaf.out[i]) edges.emplace_back(up[i], up[static_cast<std::size_t>(j)]);
    }
    const auto lifted = detail::make_graph(up, edges);
    const auto lifted_reach = detail::reach(lifted, false);
    const std::size_t m = lifted.nodes.size();
    auto node = [&](std::size_t i) { return static_cast<std::size_t>(lifted.at.at(up[i])); };

    // Pairs a path between blocks confirms.
    std::vector<std::vector<char>> confirmed(m, std::vector<char>(m, 0));
    for (std::size_t a = 0; a < n; ++a) {
      for (std::size_t b = 0; b < n; ++b) {
        if (leaf_reach[a][b] != 0 && node(a) != node(b)) confirmed[node(a)][node(b)] = 1;
      }
    }
    LevelView lv;
    lv.level = level;
    for (std::size_t x = 0; x < m; ++x) {
      for (std::size_t y = 0; y < m; ++y) {
        if (x == y || lifted_reach[x][y] == 0) continue;
        ++lv.pairs;
        if (confirmed[x][y] != 0) {
          ++lv.true_pairs;
        } else {
          lv.false_pairs.push_back(lifted.nodes[x] + " -> " + lifted.nodes[y]);
        }
      }
    }
    std::sort(lv.false_pairs.begin(), lv.false_pairs.end());

    // A lifted cycle is real when a cycle between blocks spans at least two of its nodes.
    std::set<int> real_nodes;
    for (const auto& c : leaf_sccs) {
      std::set<int> image;
      for (int v : c) image.insert(lifted.at.at(up[static_cast<std::size_t>(v)]));
      if (image.size() >= 2) real_nodes.insert(image.begin(), image.end());
    }
    for (const auto& c : detail::sccs(lifted)) {
      if (c.size() < 2) continue;
      std::vector<std::string> ids;
      for (int v : c) ids.push_back(lifted.nodes[static_cast<std::size_t>(v)]);
      (real_nodes.contains(c.front()) ? lv.real_cycles : lv.false_cycles).push_back(std::move(ids));
    }
    std::sort(lv.real_cycles.begin(), lv.real_cycles.end());
    std::sort(lv.false_cycles.begin(), lv.false_cycles.end());
    out.levels.push_back(std::move(lv));
  }
  return out;
}

std::string to_text(const Views& v) {
  std::string out = fmt::format("@hsgm views levels={}\n", v.levels.size());
  if (v.levels.empty()) out += "nothing lifted: every code block is on the top level\n";
  for (const auto& l : v.levels) {
    out += fmt::format("level {}  precision {}/{}  false {}  cycles real {} false {}\n", l.level, l.true_pairs, l.pairs,
                       l.false_pairs.size(), l.real_cycles.size(), l.false_cycles.size());
    if (!l.false_pairs.empty()) out += "false " + detail::capped(l.false_pairs, " | ") + "\n";
    for (const auto& c : l.real_cycles) out += "real cycle " + detail::capped(c, " ") + "\n";
    for (const auto& c : l.false_cycles) out += "false cycle " + detail::capped(c, " ") + "\n";
  }
  out += "assume arrows as the views draw them (src -> dst); use links left out (3.7)\n";
  return out;
}

}  // namespace kk::hsgm
