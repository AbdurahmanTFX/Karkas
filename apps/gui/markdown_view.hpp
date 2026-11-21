#pragma once
// A note's Markdown drawn in karkas-gui (ADR 0040): headings, paragraphs, lists with task boxes, quotes,
// code, rules, tables and front matter, with links to click. The text is laid out on the draw list (word
// wrap, mixed fonts); one invisible item under it takes the mouse.
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "kk/core/markdown.hpp"

namespace kk::gui {

// What the mouse did on the view.
struct MdClick {
  std::string href;  // a link's destination, or a [[link]]'s name ("" for a double click on the text)
  bool wiki = false;
  bool image = false;
  bool dbl = false;  // a double click on the text (not on a link)
  int line = 0;      // the source line of the block under the mouse
};

// Where a link leads, for its look: kMdNowhere (dimmed), kMdNote (a note), kMdOut (a URL or a file).
inline constexpr int kMdNowhere = 0;
inline constexpr int kMdNote = 1;
inline constexpr int kMdOut = 2;
using MdLinkKind = std::function<int(const std::string& href, bool wiki)>;

// Draws `doc` at the cursor, as wide as the space left (at most `max_width` when it is set), and returns
// what was clicked. A text longer than `max_blocks` blocks is cut with "…" (the inspector).
std::optional<MdClick> markdown_view(const char* id, const std::vector<core::md::Block>& doc, const MdLinkKind& kind = {},
                                     std::size_t max_blocks = std::numeric_limits<std::size_t>::max(), float max_width = 0.0f);

}  // namespace kk::gui
