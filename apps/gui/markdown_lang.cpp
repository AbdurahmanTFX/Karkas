#include "markdown_lang.hpp"

#include <imgui.h>

#include <cstring>

namespace kk::gui {

namespace {

using Pal = TextEditor::PaletteIndex;

// The token roles, on palette entries the tokenizer never needs otherwise.
constexpr Pal kHeading = Pal::Keyword;
constexpr Pal kMarker = Pal::Number;  // list markers, task boxes
constexpr Pal kCode = Pal::String;    // inline code, fences
constexpr Pal kLink = Pal::CharLiteral;
constexpr Pal kWiki = Pal::PreprocIdentifier;
constexpr Pal kBold = Pal::KnownIdentifier;
constexpr Pal kItalic = Pal::Preprocessor;
constexpr Pal kMuted = Pal::Comment;  // quote marks, strikethrough, HTML comments
constexpr Pal kRule = Pal::Punctuation;

bool word(char c) {
  auto u = static_cast<unsigned char>(c);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || u >= 0x80;
}

bool starts(const char* p, const char* e, const char* s) {
  auto n = std::strlen(s);
  return static_cast<std::size_t>(e - p) >= n && std::memcmp(p, s, n) == 0;
}

// The end of a run of `c` at p.
const char* run_end(const char* p, const char* e, char c) {
  while (p < e && *p == c) ++p;
  return p;
}

// After a closing run of exactly `n` marks `c` (preceded by a non-space; for '_' not followed by a
// letter) that closes an opener ending at `from`; nullptr when none on the line.
const char* closing(const char* from, const char* e, char c, std::size_t n) {
  for (const char* q = from; q < e;) {
    if (*q == '\\') {
      q += 2;
      continue;
    }
    if (*q == '`') {  // code spans hide marks
      const char* r = run_end(q, e, '`');
      auto len = static_cast<std::size_t>(r - q);
      const char* k = r;
      while (k < e) {
        const char* ke = run_end(k, e, '`');
        if (ke > k && static_cast<std::size_t>(ke - k) == len) break;
        k = ke > k ? ke : k + 1;
      }
      q = k < e ? k + len : r;
      continue;
    }
    if (*q == c) {
      const char* r = run_end(q, e, c);
      auto len = static_cast<std::size_t>(r - q);
      bool prev_ok = q > from && q[-1] != ' ' && q[-1] != '\t';
      bool next_ok = c != '_' || r >= e || !word(*r);
      if (prev_ok && next_ok && (len == n || (len >= 3 && n < 3))) return q + n;
      q = r;
      continue;
    }
    ++q;
  }
  return nullptr;
}

// The end of "[label](destination)" starting at p ('['), nullptr when it is no link.
const char* link_end(const char* p, const char* e) {
  int depth = 0;
  const char* q = p;
  for (; q < e; ++q) {
    if (*q == '\\') {
      ++q;
    } else if (*q == '[') {
      ++depth;
    } else if (*q == ']' && --depth == 0) {
      break;
    }
  }
  if (q + 1 >= e || q[1] != '(') return nullptr;
  int par = 0;
  for (const char* k = q + 2; k < e; ++k) {
    if (*k == '(') {
      ++par;
    } else if (*k == ')') {
      if (par == 0) return k + 1;
      --par;
    }
  }
  return nullptr;
}

// Where the editor's colorizer is: it tokenizes line after line from the line's start, and a call at or
// before the last one (or on a buffer of another length) is a new line.
struct LineState {
  const char* begin = nullptr;
  const char* last = nullptr;
  const char* end = nullptr;
};
thread_local LineState g_line;

bool tokenize(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end, Pal& pal) {
  if (in_end != g_line.end || in_begin <= g_line.last) g_line.begin = in_begin;
  g_line.end = in_end;
  g_line.last = in_begin;
  const char* p = in_begin;
  const char* e = in_end;
  auto token = [&](const char* to, Pal kind) {
    out_begin = in_begin;
    out_end = to > in_begin ? to : in_begin + 1;
    pal = kind;
    return true;
  };
  if (p == g_line.begin) {
    const char* q = p;
    while (q < e && (*q == ' ' || *q == '\t')) ++q;
    const bool shallow = q - p <= 3;
    if (q < e && *q == '#' && shallow) {
      const char* h = run_end(q, e, '#');
      if (h - q <= 6 && (h == e || *h == ' ' || *h == '\t')) return token(e, kHeading);
    }
    if (starts(q, e, "```") || starts(q, e, "~~~")) return token(e, kCode);
    if (q < e && (*q == '-' || *q == '*' || *q == '_' || *q == '=') && shallow) {  // a rule or a setext underline
      int marks = 0;
      bool only = true;
      for (const char* k = q; k < e; ++k) {
        if (*k == *q) {
          ++marks;
        } else if (*k != ' ' && *k != '\t') {
          only = false;
          break;
        }
      }
      if (only && (marks >= 3 || (*q == '=' && marks >= 1))) return token(e, *q == '=' ? kHeading : kRule);
    }
    if (q < e && *q == '>' && shallow) {
      while (q < e && (*q == '>' || *q == ' ' || *q == '\t')) ++q;
      return token(q, kMuted);
    }
    // A list marker and its task box.
    const char* m = q;
    if (m < e && (*m == '-' || *m == '*' || *m == '+')) {
      ++m;
    } else {
      while (m < e && *m >= '0' && *m <= '9' && m - q < 9) ++m;
      m = (m > q && m < e && (*m == '.' || *m == ')')) ? m + 1 : q;
    }
    if (m > q && (m == e || *m == ' ' || *m == '\t')) {
      while (m < e && (*m == ' ' || *m == '\t')) ++m;
      if (e - m >= 3 && m[0] == '[' && (m[1] == ' ' || m[1] == 'x' || m[1] == 'X') && m[2] == ']') m += 3;
      return token(m, kMarker);
    }
  }
  const char c = *p;
  if (c == '\\' && p + 1 < e) return token(p + 2, Pal::Default);
  if (c == '`') {
    const char* r = run_end(p, e, '`');
    auto n = static_cast<std::size_t>(r - p);
    for (const char* k = r; k < e;) {
      const char* ke = run_end(k, e, '`');
      if (ke > k) {
        if (static_cast<std::size_t>(ke - k) == n) return token(ke, kCode);
        k = ke;
      } else {
        ++k;
      }
    }
    return token(r, Pal::Default);
  }
  if (starts(p, e, "[[")) {
    for (const char* k = p + 2; k + 1 < e; ++k) {
      if (k[0] == ']' && k[1] == ']') return token(k + 2, kWiki);
    }
  }
  if (c == '[' || (c == '!' && p + 1 < e && p[1] == '[')) {
    if (const char* le = link_end(c == '!' ? p + 1 : p, e)) return token(le, kLink);
  }
  if (starts(p, e, "<!--")) {
    for (const char* k = p + 4; k + 2 < e; ++k) {
      if (starts(k, e, "-->")) return token(k + 3, kMuted);
    }
    return token(e, kMuted);
  }
  if (c == '<') {
    for (const char* k = p + 1; k < e && *k != ' ' && *k != '<'; ++k) {
      if (*k == '>') {
        bool url = false;
        for (const char* t = p + 1; t < k; ++t) url = url || *t == ':' || *t == '@';
        if (url) return token(k + 1, kLink);
        break;
      }
    }
  }
  if ((starts(p, e, "https://") || starts(p, e, "http://") || starts(p, e, "www.")) && (p == g_line.begin || !word(p[-1]))) {
    const char* k = p;
    while (k < e && *k != ' ' && *k != '\t' && *k != '<') ++k;
    return token(k, kLink);
  }
  if (c == '*' || c == '_') {
    const char* r = run_end(p, e, c);
    auto n = static_cast<std::size_t>(r - p);
    bool opens = r < e && *r != ' ' && *r != '\t' && (c != '_' || p == g_line.begin || !word(p[-1]));
    if (opens && n <= 3) {
      if (const char* ce = closing(r, e, c, n)) return token(ce, n >= 2 ? kBold : kItalic);
    }
    return token(r, Pal::Default);
  }
  if (starts(p, e, "~~") && p + 2 < e && p[2] != ' ') {
    if (const char* ce = closing(p + 2, e, '~', 2)) return token(ce, kMuted);
  }
  if (word(c) || c == '_') {  // a word as a whole: '_' inside it is no mark
    const char* k = p;
    while (k < e && (word(*k) || *k == '_')) ++k;
    return token(k, Pal::Default);
  }
  return false;
}

}  // namespace

const TextEditor::LanguageDefinition& markdown_language() {
  static const TextEditor::LanguageDefinition def = [] {
    TextEditor::LanguageDefinition d;
    d.mName = "Markdown";
    d.mTokenize = &tokenize;
    // No comments, strings or preprocessor lines of a programming language.
    d.mCommentStart = "\x01\x02";
    d.mCommentEnd = "\x02\x01";
    d.mSingleLineComment = "";
    d.mPreprocChar = '\x01';
    d.mAutoIndentation = true;
    d.mCaseSensitive = true;
    return d;
  }();
  return def;
}

TextEditor::Palette markdown_palette(TextEditor::Palette p) {
  auto set = [&](Pal i, ImU32 c) { p[static_cast<std::size_t>(i)] = c; };
  set(kHeading, IM_COL32(86, 156, 214, 255));
  set(kMarker, IM_COL32(215, 186, 125, 255));
  set(kCode, IM_COL32(206, 145, 120, 255));
  set(kLink, IM_COL32(78, 201, 176, 255));
  set(kWiki, IM_COL32(120, 170, 240, 255));
  set(kBold, IM_COL32(245, 245, 245, 255));
  set(kItalic, IM_COL32(197, 192, 230, 255));
  set(kMuted, IM_COL32(125, 132, 142, 255));
  set(kRule, IM_COL32(110, 118, 129, 255));
  return p;
}

}  // namespace kk::gui
