#include "kk/core/markdown.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "kk/core/text.hpp"

namespace kk::core::md {

namespace {

constexpr auto npos = std::string_view::npos;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c); }
// Letters and digits of any script: the bytes of non-ASCII UTF-8 count as word characters.
bool is_word(char c) { return is_alnum(c) || (static_cast<unsigned char>(c) & 0x80) != 0; }
bool is_punct(char c) { return c > 32 && c < 127 && !is_alnum(c); }

std::string_view trim_left(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

std::string_view trim_right(std::string_view s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Columns of leading white space (a tab goes to the next multiple of 4).
int indent_of(std::string_view s) {
  int col = 0;
  for (char c : s) {
    if (c == ' ') {
      ++col;
    } else if (c == '\t') {
      col += 4 - col % 4;
    } else {
      break;
    }
  }
  return col;
}

// The line without up to `cols` columns of leading white space.
std::string_view strip_indent(std::string_view s, int cols) {
  int col = 0;
  std::size_t i = 0;
  while (i < s.size() && col < cols && (s[i] == ' ' || s[i] == '\t')) {
    col += s[i] == '\t' ? 4 - col % 4 : 1;
    ++i;
  }
  return s.substr(i);
}

bool blank(std::string_view s) { return trim(s).empty(); }

// Backslash escapes of ASCII punctuation removed.
std::string unescape(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && is_punct(s[i + 1])) ++i;
    out += s[i];
  }
  return out;
}

bool is_email(std::string_view s) {
  auto at = s.find('@');
  return at != npos && at > 0 && s.find('.', at) != npos && s.find_first_of(" \t<>") == npos;
}

// --- inline ---------------------------------------------------------------------------------------

// The end of the code span whose backticks start at i (just after its closing backticks); npos when no
// run of as many backticks closes it. `open` gets the number of backticks.
std::size_t code_span_end(std::string_view s, std::size_t i, std::size_t* open = nullptr) {
  std::size_t n = 0;
  while (i + n < s.size() && s[i + n] == '`') ++n;
  if (open != nullptr) *open = n;
  for (std::size_t j = i + n; j < s.size();) {
    if (s[j] != '`') {
      ++j;
      continue;
    }
    std::size_t m = 0;
    while (j + m < s.size() && s[j + m] == '`') ++m;
    if (m == n) return j + m;
    j += m;
  }
  return npos;
}

struct LinkAt {
  std::string_view label;
  std::string href;
  std::size_t end = 0;  // just after ')'
};

// A [label](destination "title") starting at s[i] == '['.
std::optional<LinkAt> link_at(std::string_view s, std::size_t i) {
  int depth = 0;
  std::size_t j = i;
  for (; j < s.size(); ++j) {
    char c = s[j];
    if (c == '\\') {
      ++j;
    } else if (c == '`') {
      auto e = code_span_end(s, j);
      if (e != npos) {
        j = e - 1;
      } else {
        while (j + 1 < s.size() && s[j + 1] == '`') ++j;
      }
    } else if (c == '[') {
      ++depth;
    } else if (c == ']' && --depth == 0) {
      break;
    }
  }
  if (j + 1 >= s.size() || s[j + 1] != '(') return std::nullopt;
  LinkAt out;
  out.label = s.substr(i + 1, j - i - 1);
  std::size_t k = j + 2;
  while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
  if (k < s.size() && s[k] == '<') {
    auto e = s.find_first_of(">\n", k + 1);
    if (e == npos || s[e] != '>') return std::nullopt;
    out.href = unescape(s.substr(k + 1, e - k - 1));
    k = e + 1;
  } else {
    int par = 0;
    std::size_t b = k;
    for (; k < s.size(); ++k) {
      char c = s[k];
      if (c == '\\' && k + 1 < s.size() && is_punct(s[k + 1])) {
        ++k;
      } else if (is_space(c)) {
        break;
      } else if (c == '(') {
        ++par;
      } else if (c == ')') {
        if (par == 0) break;
        --par;
      }
    }
    out.href = unescape(s.substr(b, k - b));
  }
  std::size_t before_title = k;
  while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
  if (k > before_title && k < s.size() && (s[k] == '"' || s[k] == '\'' || s[k] == '(')) {
    char close = s[k] == '(' ? ')' : s[k];
    auto e = s.find(close, k + 1);
    if (e == npos) return std::nullopt;
    k = e + 1;
    while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
  }
  if (k >= s.size() || s[k] != ')') return std::nullopt;
  out.end = k + 1;
  return out;
}

struct Style {
  bool bold = false;
  bool italic = false;
  bool strike = false;
  std::string href;
};

void emit(std::vector<Run>& out, std::string_view text, const Style& st, bool code = false) {
  if (text.empty()) return;
  if (!out.empty()) {
    auto& b = out.back();
    if (b.text != "\n" && b.bold == st.bold && b.italic == st.italic && b.strike == st.strike && b.code == code && !b.image &&
        !b.wiki && b.href == st.href) {
      b.text += text;
      return;
    }
  }
  Run r;
  r.text = std::string(text);
  r.bold = st.bold;
  r.italic = st.italic;
  r.strike = st.strike;
  r.code = code;
  r.href = st.href;
  out.push_back(std::move(r));
}

// The closing run of `c` for an opener of `len` at `from`: its position and length.
std::optional<std::pair<std::size_t, std::size_t>> closer(std::string_view s, std::size_t from, char c, std::size_t len) {
  for (std::size_t k = from; k < s.size();) {
    char x = s[k];
    if (x == '\\') {
      k += 2;
      continue;
    }
    if (x == '`') {
      auto e = code_span_end(s, k);
      if (e != npos) {
        k = e;
      } else {
        while (k < s.size() && s[k] == '`') ++k;
      }
      continue;
    }
    if (x == '[' && k + 1 < s.size() && s[k + 1] == '[') {
      if (auto e = s.find("]]", k + 2); e != npos) {
        k = e + 2;
        continue;
      }
    }
    if (x == ']' && k + 1 < s.size() && s[k + 1] == '(') {  // a link's destination
      if (auto e = s.find(')', k + 2); e != npos) {
        k = e + 1;
        continue;
      }
    }
    if (x == c) {
      std::size_t m = 0;
      while (k + m < s.size() && s[k + m] == c) ++m;
      bool prev_ok = k > from && !is_space(s[k - 1]);
      bool next_ok = c != '_' || k + m >= s.size() || !is_word(s[k + m]);
      bool fits = c == '~' ? m == 2 : (m == len || m >= 3 || len >= 3);
      if (prev_ok && next_ok && fits) return std::make_pair(k, m);
      k += m;
      continue;
    }
    ++k;
  }
  return std::nullopt;
}

void inline_into(std::string_view s, const Style& st, std::vector<Run>& out) {
  std::string buf;
  auto flush = [&] {
    emit(out, buf, st);
    buf.clear();
  };
  const std::size_t n = s.size();
  std::size_t i = 0;
  while (i < n) {
    char c = s[i];
    if (c == '\n') {
      flush();
      Run br;
      br.text = "\n";
      out.push_back(std::move(br));
      ++i;
      continue;
    }
    if (c == '\\' && i + 1 < n && is_punct(s[i + 1])) {
      buf += s[i + 1];
      i += 2;
      continue;
    }
    if (c == '`') {
      std::size_t open = 0;
      auto e = code_span_end(s, i, &open);
      if (e == npos) {
        while (i < n && s[i] == '`') buf += s[i++];
        continue;
      }
      std::string code(s.substr(i + open, e - open - (i + open)));
      std::replace(code.begin(), code.end(), '\n', ' ');
      if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos) {
        code = code.substr(1, code.size() - 2);
      }
      flush();
      emit(out, code, st, true);
      i = e;
      continue;
    }
    if (c == '[' && i + 1 < n && s[i + 1] == '[') {
      auto e = s.find("]]", i + 2);
      auto nl = s.find('\n', i + 2);
      if (e != npos && (nl == npos || nl > e)) {
        auto inner = s.substr(i + 2, e - i - 2);
        auto bar = inner.find('|');
        auto name = trim(bar == npos ? inner : inner.substr(0, bar));
        auto shown = bar == npos ? name : trim(inner.substr(bar + 1));
        if (!name.empty()) {
          flush();
          Run r;
          r.text = std::string(shown.empty() ? name : shown);
          r.bold = st.bold;
          r.italic = st.italic;
          r.strike = st.strike;
          r.wiki = true;
          r.href = std::string(name);
          out.push_back(std::move(r));
          i = e + 2;
          continue;
        }
      }
    }
    if (c == '!' && i + 1 < n && s[i + 1] == '[') {
      if (auto l = link_at(s, i + 1)) {
        flush();
        std::vector<Run> alt;
        inline_into(l->label, Style{}, alt);
        Run r;
        r.text = plain(alt);
        r.image = true;
        r.href = l->href;
        out.push_back(std::move(r));
        i = l->end;
        continue;
      }
    }
    if (c == '[') {
      if (auto l = link_at(s, i)) {
        flush();
        Style in = st;
        in.href = l->href;
        if (trim(l->label).empty()) {
          emit(out, l->href, in);  // nothing to show: the destination
        } else {
          inline_into(l->label, in, out);
        }
        i = l->end;
        continue;
      }
    }
    if (c == '<') {
      if (auto e = s.find('>', i + 1); e != npos) {
        auto inner = s.substr(i + 1, e - i - 1);
        if (!inner.empty() && inner.find_first_of(" \t\n<") == npos && (is_url(inner) || is_email(inner))) {
          flush();
          Style l = st;
          l.href = is_url(inner) ? std::string(inner) : "mailto:" + std::string(inner);
          emit(out, inner, l);
          i = e + 1;
          continue;
        }
      }
    }
    if (st.href.empty() && (c == 'h' || c == 'w') && (i == 0 || !is_word(s[i - 1]))) {
      auto rest = s.substr(i);
      bool www = rest.starts_with("www.");
      if (www || rest.starts_with("https://") || rest.starts_with("http://")) {
        std::size_t e = i;
        while (e < n && !is_space(s[e]) && s[e] != '<') ++e;
        // Trailing punctuation belongs to the sentence, an unbalanced ')' to the text around.
        while (e > i) {
          char t = s[e - 1];
          if (std::string_view("?!.,:;*_~'\"").find(t) != npos) {
            --e;
            continue;
          }
          if (t == ')') {
            auto url = s.substr(i, e - i);
            if (std::count(url.begin(), url.end(), '(') < std::count(url.begin(), url.end(), ')')) {
              --e;
              continue;
            }
          }
          break;
        }
        auto url = s.substr(i, e - i);
        std::size_t prefix = www ? 4 : rest.starts_with("https://") ? 8 : 7;
        if (url.size() > prefix) {
          flush();
          Style l = st;
          l.href = www ? "https://" + std::string(url) : std::string(url);
          emit(out, url, l);
          i = e;
          continue;
        }
      }
    }
    if (c == '*' || c == '_' || c == '~') {
      std::size_t len = 0;
      while (i + len < n && s[i + len] == c) ++len;
      bool next_ok = i + len < n && !is_space(s[i + len]);
      bool open = next_ok && (c != '_' || i == 0 || !is_word(s[i - 1])) && (c != '~' || len == 2);
      if (open) {
        if (auto cl = closer(s, i + len, c, len)) {
          auto [at, clen] = *cl;
          std::size_t use = c == '~' ? 2 : std::min<std::size_t>({len, clen, 3});
          buf.append(len - use, c);  // opening marks beyond the ones used stay text
          flush();
          Style in = st;
          if (c == '~') {
            in.strike = true;
          } else {
            in.italic = in.italic || use != 2;
            in.bold = in.bold || use >= 2;
          }
          inline_into(s.substr(i + len, at - (i + len)), in, out);
          buf.append(clen - use, c);
          i = at + clen;
          continue;
        }
      }
      buf.append(len, c);
      i += len;
      continue;
    }
    buf += c;
    ++i;
  }
  flush();
}

// --- blocks ---------------------------------------------------------------------------------------

struct Line {
  std::string_view text;
  int no = 0;
};

std::vector<Line> split_lines(std::string_view text) {
  std::vector<Line> out;
  std::size_t pos = 0;
  int no = 1;
  while (pos <= text.size()) {
    auto e = text.find('\n', pos);
    if (e == npos) e = text.size();
    auto l = text.substr(pos, e - pos);
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    out.push_back({l, no++});
    if (e == text.size()) break;
    pos = e + 1;
  }
  if (!out.empty() && out.back().text.empty()) out.pop_back();  // the text's final newline
  return out;
}

// "# Title ##" → level 1, "Title".
bool atx(std::string_view t, int& level, std::string_view& content) {
  std::size_t n = 0;
  while (n < t.size() && t[n] == '#') ++n;
  if (n == 0 || n > 6 || (n < t.size() && t[n] != ' ' && t[n] != '\t')) return false;
  level = static_cast<int>(n);
  auto c = trim(t.substr(n));
  // A closing run of '#' after a space (or the whole content) goes.
  std::size_t e = c.size();
  while (e > 0 && c[e - 1] == '#') --e;
  if (e == 0 || c[e - 1] == ' ' || c[e - 1] == '\t') c = trim(c.substr(0, e));
  content = c;
  return true;
}

bool fence_open(std::string_view t, char& ch, std::size_t& len, std::string_view& info) {
  if (t.empty() || (t[0] != '`' && t[0] != '~')) return false;
  ch = t[0];
  len = 0;
  while (len < t.size() && t[len] == ch) ++len;
  if (len < 3) return false;
  info = trim(t.substr(len));
  return ch != '`' || info.find('`') == npos;
}

bool fence_close(std::string_view t, char ch, std::size_t len) {
  std::size_t n = 0;
  while (n < t.size() && t[n] == ch) ++n;
  return n >= len && blank(t.substr(n));
}

// ---, ***, ___ (three or more, spaces allowed between).
bool rule(std::string_view t) {
  t = trim(t);
  if (t.empty() || (t[0] != '-' && t[0] != '*' && t[0] != '_')) return false;
  int n = 0;
  for (char c : t) {
    if (c == t[0]) {
      ++n;
    } else if (c != ' ' && c != '\t') {
      return false;
    }
  }
  return n >= 3;
}

// A setext underline: '=' (level 1) or '-' (level 2, at least two) and nothing else.
int setext(std::string_view t) {
  t = trim(t);
  if (t.empty() || (t[0] != '=' && t[0] != '-')) return 0;
  if (t.find_first_not_of(t[0]) != npos) return 0;
  if (t[0] == '-' && t.size() < 2) return 0;
  return t[0] == '=' ? 1 : 2;
}

struct ItemStart {
  std::string marker;
  int number = 0;
  std::string_view content;
  int task = -1;
};

std::optional<ItemStart> item_start(std::string_view t) {
  ItemStart out;
  std::size_t k = 0;
  if (!t.empty() && (t[0] == '-' || t[0] == '*' || t[0] == '+')) {
    out.marker = "-";
    k = 1;
  } else {
    while (k < t.size() && k < 9 && is_digit(t[k])) ++k;
    if (k == 0 || k >= t.size() || (t[k] != '.' && t[k] != ')')) return std::nullopt;
    for (std::size_t d = 0; d < k; ++d) out.number = out.number * 10 + (t[d] - '0');
    out.marker = std::string(t.substr(0, k + 1));
    ++k;
  }
  if (k < t.size() && t[k] != ' ' && t[k] != '\t') return std::nullopt;
  auto c = trim(t.substr(k));
  if (c.size() >= 3 && c[0] == '[' && c[2] == ']' && (c[1] == ' ' || c[1] == 'x' || c[1] == 'X') &&
      (c.size() == 3 || c[3] == ' ' || c[3] == '\t')) {
    out.task = c[1] == ' ' ? 0 : 1;
    c = trim(c.substr(3));
  }
  out.content = c;
  return out;
}

// The cells of a table row: outer pipes dropped, split at unescaped '|', "\|" kept as '|'.
std::vector<std::string> cells(std::string_view row) {
  row = trim(row);
  if (!row.empty() && row[0] == '|') row.remove_prefix(1);
  if (!row.empty() && row.back() == '|' && (row.size() < 2 || row[row.size() - 2] != '\\')) row.remove_suffix(1);
  std::vector<std::string> out;
  std::string cur;
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (row[i] == '\\' && i + 1 < row.size() && row[i + 1] == '|') {
      cur += '|';
      ++i;
    } else if (row[i] == '|') {
      out.emplace_back(trim(cur));
      cur.clear();
    } else {
      cur += row[i];
    }
  }
  out.emplace_back(trim(cur));
  return out;
}

bool delimiter_row(std::string_view row, std::vector<char>* align) {
  if (row.find('|') == npos || row.find('-') == npos) return false;
  auto cs = cells(row);
  std::vector<char> a;
  for (const auto& c : cs) {
    if (c.empty()) return false;
    bool left = c.front() == ':';
    bool right = c.back() == ':';
    auto dashes = std::string_view(c).substr(left ? 1 : 0, c.size() - (left ? 1 : 0) - (right ? 1 : 0));
    if (dashes.empty() || dashes.find_first_not_of('-') != npos) return false;
    a.push_back(left && right ? 'c' : right ? 'r' : left ? 'l' : 0);
  }
  if (align != nullptr) *align = std::move(a);
  return true;
}

void parse_lines(const std::vector<Line>& lines, int quote, bool top, std::vector<Block>& out) {
  std::optional<Block> cur;  // the paragraph or item whose text is being gathered
  std::string cur_text;
  std::vector<int> lists;  // the indent of the marker of each open list level
  bool blank_before = false;
  auto flush = [&] {
    if (!cur) return;
    cur->runs = parse_inline(cur_text);
    out.push_back(std::move(*cur));
    cur.reset();
    cur_text.clear();
  };
  auto push = [&](Block b, int line) {
    b.quote = quote;
    b.line = line;
    out.push_back(std::move(b));
  };
  // How many open list levels a line indented by `ind` belongs inside.
  auto inside = [&](int ind) {
    std::size_t d = 0;
    while (d < lists.size() && ind >= lists[d] + 2) ++d;
    return d;
  };
  for (std::size_t i = 0; i < lines.size();) {
    auto raw = lines[i].text;
    int no = lines[i].no;
    if (blank(raw)) {
      flush();
      blank_before = true;
      ++i;
      continue;
    }
    int ind = indent_of(raw);
    auto t = trim_right(trim_left(raw));
    // Front matter: "---" on the first line, closed by "---" or "...".
    if (top && i == 0 && t == "---") {
      std::size_t j = 1;
      while (j < lines.size() && trim(lines[j].text) != "---" && trim(lines[j].text) != "...") ++j;
      if (j < lines.size()) {
        Block b;
        b.kind = Kind::Meta;
        for (std::size_t k = 1; k < j; ++k) b.code += std::string(lines[k].text) + (k + 1 < j ? "\n" : "");
        push(std::move(b), no);
        i = j + 1;
        continue;
      }
    }
    char fch = 0;
    std::size_t flen = 0;
    std::string_view info;
    if ((ind <= 3 || !lists.empty()) && fence_open(t, fch, flen, info)) {
      flush();
      auto d = inside(ind);
      lists.resize(d);
      Block b;
      b.kind = Kind::Code;
      b.depth = static_cast<int>(d);
      b.lang = std::string(info.substr(0, info.find_first_of(" \t")));
      std::size_t j = i + 1;
      for (; j < lines.size() && !fence_close(trim_left(lines[j].text), fch, flen); ++j) {
        if (j > i + 1) b.code += '\n';
        b.code += std::string(strip_indent(lines[j].text, ind));
      }
      push(std::move(b), no);
      i = j + 1;
      blank_before = false;
      continue;
    }
    int level = 0;
    std::string_view content;
    if (ind <= 3 && atx(t, level, content)) {
      flush();
      lists.clear();
      Block b;
      b.kind = Kind::Heading;
      b.level = level;
      b.runs = parse_inline(content);
      push(std::move(b), no);
      ++i;
      blank_before = false;
      continue;
    }
    if (cur && cur->kind == Kind::Paragraph && !blank_before && ind <= 3) {
      if (int lv = setext(t); lv > 0) {
        cur->kind = Kind::Heading;
        cur->level = lv;
        flush();
        ++i;
        continue;
      }
    }
    if (ind <= 3 && rule(t)) {
      flush();
      lists.clear();
      Block b;
      b.kind = Kind::Rule;
      push(std::move(b), no);
      ++i;
      blank_before = false;
      continue;
    }
    if (ind <= 3 && t.starts_with('>')) {
      flush();
      lists.clear();
      std::vector<Line> sub;
      std::size_t j = i;
      for (; j < lines.size(); ++j) {
        auto q = trim_left(lines[j].text);
        if (indent_of(lines[j].text) > 3 || !q.starts_with('>')) break;
        q.remove_prefix(1);
        if (!q.empty() && (q[0] == ' ' || q[0] == '\t')) q.remove_prefix(1);
        sub.push_back({q, lines[j].no});
      }
      parse_lines(sub, quote + 1, false, out);
      i = j;
      blank_before = false;
      continue;
    }
    std::vector<char> align;
    if (t.find('|') != npos && i + 1 < lines.size() && delimiter_row(lines[i + 1].text, &align) &&
        cells(t).size() == align.size()) {
      flush();
      lists.clear();
      Block b;
      b.kind = Kind::Table;
      b.align = align;
      auto row = [&](std::string_view r) {
        auto cs = cells(r);
        cs.resize(align.size());
        std::vector<std::vector<Run>> rr;
        for (const auto& c : cs) rr.push_back(parse_inline(c));
        b.rows.push_back(std::move(rr));
      };
      row(t);
      std::size_t j = i + 2;
      for (; j < lines.size() && !blank(lines[j].text) && lines[j].text.find('|') != npos; ++j) row(lines[j].text);
      push(std::move(b), no);
      i = j;
      blank_before = false;
      continue;
    }
    // Indented code: four columns in, outside lists, not continuing a paragraph.
    if (lists.empty() && ind >= 4 && !(cur && !blank_before)) {
      flush();
      std::size_t j = i;
      std::size_t last = i;
      for (; j < lines.size() && (blank(lines[j].text) || indent_of(lines[j].text) >= 4); ++j) {
        if (!blank(lines[j].text)) last = j;
      }
      Block b;
      b.kind = Kind::Code;
      for (std::size_t k = i; k <= last; ++k) b.code += std::string(strip_indent(lines[k].text, 4)) + (k < last ? "\n" : "");
      push(std::move(b), no);
      i = last + 1;
      blank_before = false;
      continue;
    }
    if (auto it = item_start(t)) {
      // Only a non-empty bullet or an ordered item starting at 1 interrupts a paragraph.
      bool interrupts = !(cur && cur->kind == Kind::Paragraph && !blank_before) ||
                        (!it->content.empty() && (it->marker == "-" || it->number == 1));
      if (interrupts) {
        flush();
        while (!lists.empty() && ind < lists.back() + 2) lists.pop_back();
        Block b;
        b.kind = Kind::Item;
        b.depth = static_cast<int>(lists.size());
        b.marker = it->marker;
        b.task = it->task;
        b.quote = quote;
        b.line = no;
        lists.push_back(ind);
        cur = std::move(b);
        cur_text = std::string(it->content);
        ++i;
        blank_before = false;
        continue;
      }
    }
    if (cur && !blank_before) {
      // The text goes on: a trailing backslash was a line break, and every line break stays one.
      if (cur_text.ends_with('\\')) cur_text.pop_back();
      cur_text += '\n';
      cur_text += t;
    } else {
      flush();
      auto d = inside(ind);
      lists.resize(d);
      Block b;
      b.kind = Kind::Paragraph;
      b.depth = static_cast<int>(d);
      b.quote = quote;
      b.line = no;
      cur = std::move(b);
      cur_text = std::string(t);
    }
    ++i;
    blank_before = false;
  }
  flush();
}

}  // namespace

std::vector<Run> parse_inline(std::string_view text) {
  std::vector<Run> out;
  inline_into(text, Style{}, out);
  return out;
}

std::vector<Block> parse(std::string_view text) {
  std::vector<Block> out;
  parse_lines(split_lines(text), 0, true, out);
  return out;
}

std::vector<Link> links(std::string_view text) {
  std::vector<Link> out;
  auto lines = split_lines(text);
  std::size_t skip_to = 0;  // front matter: its lines hold no links
  if (!lines.empty() && trim_right(lines[0].text) == "---") {
    for (std::size_t j = 1; j < lines.size(); ++j) {
      if (trim(lines[j].text) == "---" || trim(lines[j].text) == "...") {
        skip_to = j + 1;
        break;
      }
    }
  }
  bool fenced = false;
  char fch = 0;
  std::size_t flen = 0;
  for (std::size_t li = skip_to; li < lines.size(); ++li) {
    auto line = lines[li].text;
    auto t = trim_left(line);
    if (fenced) {
      fenced = !fence_close(t, fch, flen);
      continue;
    }
    std::string_view info;
    if (fence_open(t, fch, flen, info)) {
      fenced = true;
      continue;
    }
    const auto base = static_cast<std::size_t>(line.data() - text.data());
    for (std::size_t k = 0; k < line.size();) {
      char c = line[k];
      if (c == '\\') {
        k += 2;
      } else if (c == '`') {
        auto e = code_span_end(line, k);
        if (e != npos) {
          k = e;
        } else {
          while (k < line.size() && line[k] == '`') ++k;
        }
      } else if (c == '[' && k + 1 < line.size() && line[k + 1] == '[') {
        auto e = line.find("]]", k + 2);
        k = e == npos ? k + 2 : e + 2;
      } else if (c == '!' && k + 1 < line.size() && line[k + 1] == '[') {
        auto l = link_at(line, k + 1);
        k = l ? l->end : k + 2;
      } else if (c == '[') {
        if (auto l = link_at(line, k)) {
          out.push_back({std::string(l->label), l->href, lines[li].no, base + k, base + l->end});
          k = l->end;
        } else {
          ++k;
        }
      } else {
        ++k;
      }
    }
  }
  return out;
}

std::string plain(const std::vector<Run>& runs) {
  std::string out;
  for (const auto& r : runs) out += r.text;
  return out;
}

std::vector<std::string> plain_lines(std::string_view text, std::size_t max) {
  std::vector<std::string> out;
  // A piece of text, its first line after `first`, the lines after it after `rest`.
  auto add = [&](const std::string& first, const std::string& rest, const std::string& s) {
    bool at_first = true;
    for (const auto& l : split(s, '\n')) {
      if (out.size() >= max) return;
      auto tl = trim(l);
      if (tl.empty()) continue;
      out.push_back((at_first ? first : rest) + std::string(tl));
      at_first = false;
    }
  };
  for (const auto& b : parse(text)) {
    if (out.size() >= max) break;
    std::string pad(static_cast<std::size_t>(b.depth) * 2, ' ');
    switch (b.kind) {
      case Kind::Heading:
      case Kind::Paragraph: add(pad, pad, plain(b.runs)); break;
      case Kind::Item: {
        std::string m = b.task == 0 ? "☐ " : b.task == 1 ? "☑ " : b.marker == "-" ? "• " : b.marker + " ";
        add(pad + m, pad + "  ", plain(b.runs));
        break;
      }
      case Kind::Code: add(pad, pad, b.code); break;
      case Kind::Table:
        for (const auto& row : b.rows) {
          std::vector<std::string> cs;
          for (const auto& c : row) cs.push_back(plain(c));
          add("", "", join(cs, " | "));
        }
        break;
      case Kind::Rule:
      case Kind::Meta: break;
    }
  }
  if (out.size() > max) out.resize(max);
  return out;
}

bool is_markdown_file(std::string_view path) {
  auto l = to_lower(path);
  return l.ends_with(".md") || l.ends_with(".markdown");
}

bool is_url(std::string_view href) {
  if (href.starts_with("//") || href.starts_with("www.")) return true;
  std::size_t k = 0;
  if (href.empty() || !((href[0] >= 'a' && href[0] <= 'z') || (href[0] >= 'A' && href[0] <= 'Z'))) return false;
  while (k < href.size() && (is_alnum(href[k]) || href[k] == '+' || href[k] == '.' || href[k] == '-')) ++k;
  return k >= 2 && k < href.size() && href[k] == ':';
}

std::string percent_decode(std::string_view s) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
      out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace kk::core::md
