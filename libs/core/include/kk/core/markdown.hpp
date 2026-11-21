#pragma once
// Markdown of notes (docs/adr/0040): a small parser of its own, no dependency, for what notes use —
// headings, paragraphs, lists with task boxes, quotes, fenced and indented code, rules, tables, front
// matter; inline bold, italic, strikethrough, code, links, images (and Obsidian ![[embeds]], docs/adr/0041),
// [[links]], <autolinks> and bare URLs.
// A line break in the text stays a line break (as in note apps), not a space. Deterministic: it shows
// notes, reads their links and gives the plain first lines drawn on the schema.
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace kk::core::md {

// A piece of text with one look.
struct Run {
  std::string text;  // "\n" alone: a line break
  bool bold = false;
  bool italic = false;
  bool strike = false;
  bool code = false;
  bool image = false;  // ![alt](src) or ![[file.png]]: text is the alt text (the file name), href the source
  bool wiki = false;   // [[name]] or [[name|shown]]: text is what is shown, href the name; with image: ![[file.png]]
  std::string href;    // a link's destination as written ("" for plain text)
  int width = 0;       // an image: the width asked for in pixels ("|300" after its alt text or name), 0 its own
  bool operator==(const Run&) const = default;
};

enum class Kind { Paragraph, Heading, Item, Code, Rule, Table, Meta };

struct Block {
  Kind kind = Kind::Paragraph;
  int level = 0;       // Heading: 1..6
  int depth = 0;       // lists: an item's nesting (0 = top); a paragraph or code inside an item: its depth + 1
  int quote = 0;       // how many '>' it is inside
  std::string marker;  // Item: "-" for any bullet, else the number with its '.' or ')'
  int task = -1;       // Item: -1 no box, 0 "[ ]", 1 "[x]"
  std::vector<Run> runs;  // Paragraph, Heading, Item
  std::string code;       // Code, Meta: the lines as written
  std::string lang;       // Code: the word after the opening fence
  std::vector<std::vector<std::vector<Run>>> rows;  // Table: rows of cells, rows[0] the header
  std::vector<char> align;                          // Table: per column 'l', 'c', 'r' or 0
  int line = 0;                                     // where it starts in the text, 1-based
};

std::vector<Block> parse(std::string_view text);
std::vector<Run> parse_inline(std::string_view text);

// A [label](destination) of a text outside code (images left out): the label and destination as written,
// [begin, end) the bytes of the whole link.
struct Link {
  std::string label, href;
  int line = 0;
  std::size_t begin = 0, end = 0;
};
std::vector<Link> links(std::string_view text);

// An image of a text outside code (docs/adr/0041): ![alt](src "title"), or ![[file.png|300]] (an Obsidian
// embed of an image file; an embed of anything else is a [[link]]).
struct Image {
  std::string alt, src;
  bool wiki = false;
  int width = 0;
  int line = 0;
};
std::vector<Image> images(std::string_view text);
// A file name with an image extension: .png .jpg .jpeg .gif .bmp .webp .svg .avif (any case).
bool is_image_file(std::string_view path);

// The text without its markup, one line per heading, paragraph line, item ("• ", "1. ", "☐ ", "☑ "),
// code line and table row (cells joined by " | "); blank lines, rules and front matter left out.
std::vector<std::string> plain_lines(std::string_view text, std::size_t max = std::numeric_limits<std::size_t>::max());
// The text of runs without markup (an image gives its alt text).
std::string plain(const std::vector<Run>& runs);

// A file name ending in .md or .markdown (any case).
bool is_markdown_file(std::string_view path);
// A destination that leaves the project: a scheme ("https:", "mailto:"; not a drive letter), "//host" or "www.".
bool is_url(std::string_view href);
// "%20" and the like decoded (a broken escape stays as written).
std::string percent_decode(std::string_view s);

}  // namespace kk::core::md
