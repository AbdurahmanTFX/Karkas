#pragma once
// KSL (Karkas Schema Language) — generic line-oriented record layer (4.2, 4.5).
//
// A file is a sequence of records. A record is a header line "@<kind> <id> [attrs]"
// followed by directive lines and optional sections ("--- <name>"). This layer knows
// the lexical rules only; typed records (block, project, ticket, ...) are built on top
// of it in model.hpp with the directive table.
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/error.hpp"

namespace kk::core {

// Comments that belong to an element: full-line comments right before it and the
// trailing comment on the same line. Kept so that rewriting a file is lossless.
struct Trivia {
  std::vector<std::string> lead;  // text after '#', trimmed, one per comment line
  std::string trailing;           // text after '#', trimmed; empty = none
  bool has_trailing = false;
  [[nodiscard]] bool empty() const { return lead.empty() && !has_trailing; }
};

struct Line {
  std::string directive;  // "purpose", "in", "L0", or "+", "-", "~" in a diff section
  std::string text;       // argument text, comment stripped and trimmed
  Trivia trivia;
  int line = 0;      // 1-based line number
  int col = 0;       // column of the directive
  int text_col = 0;  // column where `text` starts
};

struct Section {
  std::string name;
  std::vector<Line> lines;
  Trivia trivia;
  int line = 0;
};

struct Attr {
  std::string key;
  std::string value;
};

struct Record {
  std::string kind;         // project | block | ticket | report | change | view
  std::string id;           // path or record id
  std::vector<Attr> attrs;  // header attributes
  Trivia trivia;            // comments before / on the header line
  std::vector<Line> lines;
  std::vector<Section> sections;
  std::vector<std::string> tail;  // comment lines after the last element
  std::string file;
  int line = 0;
};

// One argument token of a directive line.
struct Token {
  enum class Kind { Word, Attr, Quoted };
  Kind kind = Kind::Word;
  std::string text;   // Word/Quoted: the value; Attr: the whole "k=v" source
  std::string key;    // Attr only
  std::string value;  // Attr only (unquoted)
  int col = 0;        // 1-based column in the source line
};

// Lexing / parsing ----------------------------------------------------------------
// Parses all records of a file. Syntax errors carry file:line:col.
Result<std::vector<Record>> parse_records(std::string_view text, std::string_view file);

// Splits directive arguments into tokens. Brackets (), <>, [] are balanced so that
// "open(const T& a, int b)" or "std::map<K, V>" stay one Word.
Result<std::vector<Token>> tokenize(const Line& line, std::string_view file);

// Reads a free-text argument: either the raw text, or one quoted string.
Result<std::string> free_text(const Line& line, std::string_view file);

// Writing -----------------------------------------------------------------------
// Canonical directive column width (4.8).
inline constexpr std::size_t kDirectiveWidth = 8;

// "purpose move frames ..." with the directive padded to kDirectiveWidth.
std::string format_line(std::string_view directive, std::string_view args);
// Free text written so that it parses back identically (quoted only when needed).
std::string format_free_text(std::string_view text);
std::string format_attr(std::string_view key, std::string_view value);

// Appends lead comments, the line and its trailing comment to `out`.
void emit(std::string& out, const Trivia& trivia, std::string_view line_text);

// Writes a generic record exactly in the order it holds its lines (used for views,
// diffs and records without a typed writer).
std::string write_record(const Record& rec);

}  // namespace kk::core
