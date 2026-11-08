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

// Port rows of a block: on the left in ports and fn ports (called by others), on the right
// out ports. The block's own in/out points sit on its title row.
inline std::pair<int, int> pin_rows(const index::Snapshot& s, const index::BlockRow& b) {
  if (b.kind == "group") return {0, 0};
  int left = 0;
  int right = 0;
  for (const auto* p : s.ports_of(b.id)) (p->dir == "out" ? right : left)++;
  return {left, right};
}

// Rows: name, purpose, then pins (at most 10 rows by default: blocks of imported code may
// have dozens of functions; the rest fold into "+N" until the block is made bigger).
inline std::pair<double, double> base_block_size(const std::string& id, const std::string& kind, const std::string& purpose,
                                                 int left, int right, double char_w, double line_h) {
  std::size_t chars = std::max<std::size_t>({id.size() + 4, std::min<std::size_t>(purpose.size(), 34), 18});
  double w = static_cast<double>(chars) * char_w + 28;
  int rows = kind == "group" ? 0 : std::min(std::max(left, right), 10);
  double h = (2.0 + rows) * line_h + 14;
  return {w, h};
}

}  // namespace kk::gui
