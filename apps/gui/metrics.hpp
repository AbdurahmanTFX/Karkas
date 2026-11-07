#pragma once
// Default size of a block on the schema at block scale 1 — shared by the worker (layout)
// and the UI (font scale = size / default size), so the text exactly fills a block.
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace kk::gui {

// Rows: name, purpose, then pins (groups: one row of ▶ pins; leaves: in ports + the
// implicit "links" pin on the left, out/fn ports on the right).
inline std::pair<double, double> base_block_size(const std::string& id, const std::string& kind, const std::string& purpose,
                                                 int ins, int outs, double char_w, double line_h) {
  std::size_t chars = std::max<std::size_t>({id.size() + 4, std::min<std::size_t>(purpose.size(), 34), 18});
  double w = static_cast<double>(chars) * char_w + 28;
  // At most 10 rows of pins by default: blocks of imported code may have dozens of
  // functions; the rest fold into "+N" until the block is made bigger.
  int pin_rows = kind == "group" ? 1 : std::min(std::max(ins + 1, outs), 10);
  double h = (2.0 + pin_rows) * line_h + 14;
  return {w, h};
}

}  // namespace kk::gui
