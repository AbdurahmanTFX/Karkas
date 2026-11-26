#pragma once
// Graphs over the code blocks of a project (private to kk_hsgm).
#include <map>
#include <string>
#include <vector>

#include "kk/hsgm/input.hpp"

namespace kk::hsgm::detail {

struct Graph {
  std::vector<std::string> nodes;      // sorted
  std::map<std::string, int> at;       // node → index
  std::vector<std::vector<int>> out;   // sorted, no duplicates, no self loops
};

// A graph over `nodes` (sorted and de-duplicated here) with the given edges; unknown ends and self loops dropped.
Graph make_graph(std::vector<std::string> nodes, const std::vector<std::pair<std::string, std::string>>& edges);

// Code blocks; data/event/call/state links as the views draw them (src -> dst as stored).
Graph drawn_graph(const Input& in);
// Code blocks; i -> j when i depends on j: data, event: consumer (dst) -> provider (src);
// call, use: src -> dst; state: both ways. ref links are ignored.
Graph dependency_graph(const Input& in);

// r[i][j] = 1 when j is reachable from i by a path of length >= 1 (or i == j and reflexive).
std::vector<std::vector<char>> reach(const Graph& g, bool reflexive);

// Strongly connected components; members sorted, components sorted by their first member.
std::vector<std::vector<int>> sccs(const Graph& g);

}  // namespace kk::hsgm::detail
