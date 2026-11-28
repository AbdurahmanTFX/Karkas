#pragma once
// views (ADR 0049): how true the lifted views are. Lifting links to the ancestors on a level is sound
// (a path between blocks always shows between their ancestors) but not exact: it can add pairs and cycles.
#include <string>
#include <vector>

#include "kk/hsgm/input.hpp"

namespace kk::hsgm {

struct LevelView {
  int level = 0;       // 0 = the top level
  int pairs = 0;       // ordered pairs x != y with y reachable from x in the lifted graph
  int true_pairs = 0;  // of them, those a path between blocks under x and under y confirms
  std::vector<std::string> false_pairs;                             // "x -> y", sorted
  std::vector<std::vector<std::string>> real_cycles, false_cycles;  // node ids sorted; lists sorted
};

struct Views {
  std::vector<LevelView> levels;  // empty: every code block is on the top level, nothing is lifted
};

Views views(const Input& in);
std::string to_text(const Views& v);

}  // namespace kk::hsgm
