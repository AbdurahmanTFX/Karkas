#pragma once
// Default size of a block on the schema at block scale 1 — shared by the worker (layout)
// and the UI (font scale = size / default size), so the text exactly fills a block.
#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>

#include "kk/index/index.hpp"

namespace kk::gui {

// Blocks other blocks "use" or share state with: they get a left "used" pin.
inline std::set<std::string> used_blocks(const index::Snapshot& s) {
  std::set<std::string> out;
  for (const auto& l : s.links) {
    if (l.kind == "use" || l.kind == "state") out.insert(l.dst_block);
  }
  return out;
}

// Pins of a block. Everything flows left to right, like the layout:
//   left  (inputs):  in ports, fn ports (called by others), "used" (use/state links in)
//   right (outputs): out ports, "calls" (call/use links out; not for shared blocks)
inline std::pair<int, int> pin_rows(const index::Snapshot& s, const index::BlockRow& b, const std::set<std::string>& used) {
  if (b.kind == "group") return {1, 1};
  int left = 0;
  int right = 0;
  for (const auto* p : s.ports_of(b.id)) (p->dir == "out" ? right : left)++;
  if (used.contains(b.id)) ++left;
  if (b.kind != "shared") ++right;
  return {left, right};
}

// Rows: name, purpose, then pins (at most 10 rows by default: blocks of imported code may
// have dozens of functions; the rest fold into "+N" until the block is made bigger).
inline std::pair<double, double> base_block_size(const std::string& id, const std::string& kind, const std::string& purpose,
                                                 int left, int right, double char_w, double line_h) {
  std::size_t chars = std::max<std::size_t>({id.size() + 4, std::min<std::size_t>(purpose.size(), 34), 18});
  double w = static_cast<double>(chars) * char_w + 28;
  int rows = kind == "group" ? 1 : std::min(std::max(left, right), 10);
  double h = (2.0 + rows) * line_h + 14;
  return {w, h};
}

}  // namespace kk::gui
