#include "kk/core/ksl.hpp"

#include <fmt/format.h>

#include <array>
#include <optional>

#include "kk/core/text.hpp"

namespace kk::core {

namespace {

constexpr std::array<std::string_view, 6> kRecordKinds = {"project", "block", "ticket",
                                                          "report",  "change", "view"};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

SourceLoc at(std::string_view file, int line, int col) {
  return SourceLoc{std::string(file), line, col};
}

struct Stripped {
  std::string_view content;
  bool has_comment = false;
  std::string comment;
};

// '#' starts a comment outside quotes, at line start or after whitespace (4.2).
Stripped strip_comment(std::string_view line) {
  bool in_quotes = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_quotes) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        in_quotes = false;
      }
      continue;
    }
    if (c == '"') {
      in_quotes = true;
    } else if (c == '#' && (i == 0 || is_space(line[i - 1]))) {
      return {line.substr(0, i), true, std::string(trim(line.substr(i + 1)))};
    }
  }
  return {line, false, {}};
}

std::size_t leading_ws(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size() && is_space(s[i])) ++i;
  return i;
}

// Parses a quoted string starting at s[i] == '"'. On success i points past the
// closing quote.
std::optional<std::string> parse_quoted(std::string_view s, std::size_t& i) {
  std::string out;
  ++i;
  while (i < s.size()) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.size()) {
      char n = s[i + 1];
      out.push_back(n == 'n' ? '\n' : n);
      i += 2;
      continue;
    }
    if (c == '"') {
      ++i;
      return out;
    }
    out.push_back(c);
    ++i;
  }
  return std::nullopt;
}

// Reads a word with balanced (), [], <> starting at i; stops at whitespace at depth 0.
bool read_balanced(std::string_view s, std::size_t& i, std::string& out) {
  int paren = 0;
  int square = 0;
  int angle = 0;
  std::size_t start = i;
  while (i < s.size()) {
    char c = s[i];
    if (is_space(c) && paren == 0 && square == 0 && angle == 0) break;
    if (c == '"') {
      std::size_t j = i;
      if (!parse_quoted(s, j)) return false;
      i = j;
      continue;
    }
    if (c == '(') ++paren;
    if (c == ')') --paren;
    if (c == '[') ++square;
    if (c == ']') --square;
    if (c == '<') ++angle;
    if (c == '>' && angle > 0) --angle;
    if (paren < 0 || square < 0) return false;
    ++i;
  }
  out.assign(s.substr(start, i - start));
  return paren == 0 && square == 0 && angle == 0;
}

bool is_ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

}  // namespace

Result<std::vector<Token>> tokenize(const Line& line, std::string_view file) {
  std::vector<Token> tokens;
  std::string_view s = line.text;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && is_space(s[i])) ++i;
    if (i >= s.size()) break;
    const std::size_t start = i;
    const int col = line.text_col + static_cast<int>(start);
    Token tok;
    tok.col = col;
    if (s[i] == '"') {
      auto q = parse_quoted(s, i);
      if (!q) return fail("S04", "unterminated quoted string", at(file, line.line, col));
      tok.kind = Token::Kind::Quoted;
      tok.text = *q;
    } else {
      std::size_t j = i;
      while (j < s.size() && is_ident_char(s[j])) ++j;
      if (j > i && j < s.size() && s[j] == '=' && s[i] >= 'a' && s[i] <= 'z') {
        tok.kind = Token::Kind::Attr;
        tok.key.assign(s.substr(i, j - i));
        std::size_t k = j + 1;
        if (k < s.size() && s[k] == '"') {
          auto q = parse_quoted(s, k);
          if (!q) return fail("S04", "unterminated quoted string", at(file, line.line, col));
          tok.value = *q;
        } else {
          if (!read_balanced(s, k, tok.value)) {
            return fail("S04", "unbalanced brackets in attribute value", at(file, line.line, col));
          }
          if (tok.value.empty()) {
            return fail("S04", fmt::format("empty value for attribute '{}'", tok.key),
                        at(file, line.line, col));
          }
        }
        i = k;
      } else {
        tok.kind = Token::Kind::Word;
        if (!read_balanced(s, i, tok.text)) {
          return fail("S04", "unbalanced brackets", at(file, line.line, col));
        }
      }
    }
    if (tok.kind == Token::Kind::Attr) tok.text.assign(s.substr(start, i - start));
    tokens.push_back(std::move(tok));
  }
  return tokens;
}

Result<std::string> free_text(const Line& line, std::string_view file) {
  std::string_view s = line.text;
  if (!s.empty() && s[0] == '"') {
    std::size_t i = 0;
    auto q = parse_quoted(s, i);
    if (!q) return fail("S04", "unterminated quoted string", at(file, line.line, line.text_col));
    if (!trim(s.substr(i)).empty()) {
      return fail("S04", "unexpected text after quoted string",
                  at(file, line.line, line.text_col + static_cast<int>(i)));
    }
    return *q;
  }
  return std::string(s);
}

Result<std::vector<Record>> parse_records(std::string_view text, std::string_view file) {
  std::vector<Record> recs;
  std::vector<std::string> pending;
  long cur_section = -1;
  int lineno = 0;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view raw = text.substr(pos, nl - pos);
    pos = nl + 1;
    ++lineno;
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    if (nl == text.size() && raw.empty()) break;

    Record* cur = recs.empty() ? nullptr : &recs.back();
    Section* sec = (cur != nullptr && cur_section >= 0)
                       ? &cur->sections[static_cast<std::size_t>(cur_section)]
                       : nullptr;

    // Diff lines inside "--- diff": "+ <ksl>", "- <ksl>", "~ <ksl>", taken raw.
    if (sec != nullptr && sec->name == "diff") {
      std::size_t ws = leading_ws(raw);
      std::string_view t = raw.substr(ws);
      if (!t.empty() && (t[0] == '+' || t[0] == '-' || t[0] == '~') &&
          (t.size() == 1 || is_space(t[1]))) {
        Line l;
        l.directive = std::string(1, t[0]);
        std::size_t tw = 1 + leading_ws(t.substr(1));
        l.text = std::string(trim(t.substr(1)));
        l.line = lineno;
        l.col = static_cast<int>(ws) + 1;
        l.text_col = static_cast<int>(ws + tw) + 1;
        l.trivia.lead = std::move(pending);
        pending.clear();
        sec->lines.push_back(std::move(l));
        continue;
      }
    }

    auto stripped = strip_comment(raw);
    std::size_t ws = leading_ws(stripped.content);
    std::string_view ct = trim(stripped.content);
    const int col = static_cast<int>(ws) + 1;
    if (ct.empty()) {
      if (stripped.has_comment) pending.push_back(stripped.comment);
      continue;
    }

    if (ct[0] == '@') {
      std::size_t k = 1;
      while (k < ct.size() && !is_space(ct[k])) ++k;
      std::string kind(ct.substr(1, k - 1));
      bool known = false;
      for (auto rk : kRecordKinds) known = known || rk == kind;
      if (!known) {
        return fail("S02", fmt::format("unknown record kind '@{}'", kind), at(file, lineno, col));
      }
      std::string_view rest = ct.substr(k);
      std::size_t rws = leading_ws(rest);
      rest = rest.substr(rws);
      std::size_t idend = 0;
      while (idend < rest.size() && !is_space(rest[idend])) ++idend;
      std::string id(rest.substr(0, idend));
      const int id_col = col + static_cast<int>(k + rws);
      if (id.empty()) {
        return fail("S06", fmt::format("missing id after '@{}'", kind), at(file, lineno, id_col));
      }
      bool id_ok = false;
      if (kind == "block" || kind == "project") id_ok = is_block_path(id);
      if (kind == "view") id_ok = is_block_path(id) || id == "root";
      if (kind == "ticket" || kind == "report") id_ok = is_record_id(id) && id[0] == 'T';
      if (kind == "change") id_ok = is_record_id(id) && id[0] == 'C';
      if (!id_ok) {
        return fail("S06", fmt::format("invalid id '{}' for @{}", id, kind),
                    at(file, lineno, id_col));
      }
      Record rec;
      rec.kind = kind;
      rec.id = id;
      rec.file = std::string(file);
      rec.line = lineno;
      rec.trivia.lead = std::move(pending);
      pending.clear();
      rec.trivia.has_trailing = stripped.has_comment;
      rec.trivia.trailing = stripped.comment;
      Line attr_line;
      attr_line.text = std::string(rest.substr(idend));
      attr_line.line = lineno;
      attr_line.text_col = id_col + static_cast<int>(idend);
      auto toks = tokenize(attr_line, file);
      if (!toks) return std::unexpected(toks.error());
      for (auto& t : *toks) {
        if (t.kind != Token::Kind::Attr) {
          return fail("S06", fmt::format("unexpected '{}' in record header, expected key=value", t.text),
                      at(file, lineno, t.col));
        }
        rec.attrs.push_back(Attr{t.key, t.value});
      }
      recs.push_back(std::move(rec));
      cur_section = -1;
      continue;
    }

    if (cur == nullptr) {
      return fail("S01", "expected a record header such as @block or @project",
                  at(file, lineno, col));
    }

    if (ct.starts_with("---")) {
      std::string name(trim(ct.substr(3)));
      if (!is_ident(name)) {
        return fail("S01", "malformed section header, expected '--- <name>'", at(file, lineno, col));
      }
      Section s;
      s.name = name;
      s.line = lineno;
      s.trivia.lead = std::move(pending);
      pending.clear();
      s.trivia.has_trailing = stripped.has_comment;
      s.trivia.trailing = stripped.comment;
      cur->sections.push_back(std::move(s));
      cur_section = static_cast<long>(cur->sections.size()) - 1;
      continue;
    }

    std::size_t dend = 0;
    while (dend < ct.size() && !is_space(ct[dend])) ++dend;
    Line l;
    l.directive.assign(ct.substr(0, dend));
    std::string_view rest = ct.substr(dend);
    std::size_t tws = leading_ws(rest);
    l.text.assign(trim(rest));
    l.line = lineno;
    l.col = col;
    l.text_col = col + static_cast<int>(dend + tws);
    l.trivia.lead = std::move(pending);
    pending.clear();
    l.trivia.has_trailing = stripped.has_comment;
    l.trivia.trailing = stripped.comment;
    if (sec != nullptr) {
      sec->lines.push_back(std::move(l));
    } else {
      cur->lines.push_back(std::move(l));
    }
  }
  if (!pending.empty() && !recs.empty()) recs.back().tail = std::move(pending);
  return recs;
}

// --- writing -----------------------------------------------------------------------

std::string format_line(std::string_view directive, std::string_view args) {
  if (args.empty()) return std::string(directive);
  return pad_right(directive, kDirectiveWidth) + std::string(args);
}

std::string format_free_text(std::string_view text) {
  bool q = text.empty() || text.front() == '"' || text.front() == '#' ||
           contains(text, " #") || contains(text, "\t#") || contains(text, "\n") ||
           trim(text).size() != text.size();
  return q ? quote(text) : std::string(text);
}

std::string format_attr(std::string_view key, std::string_view value) {
  return fmt::format("{}={}", key, needs_quotes(value) ? quote(value) : std::string(value));
}

void emit(std::string& out, const Trivia& trivia, std::string_view line_text) {
  for (const auto& c : trivia.lead) {
    out += c.empty() ? "#" : "# " + c;
    out += '\n';
  }
  out += line_text;
  if (trivia.has_trailing) {
    out += trivia.trailing.empty() ? "  #" : "  # " + trivia.trailing;
  }
  out += '\n';
}

std::string write_record(const Record& rec) {
  std::string out;
  std::string header = "@" + rec.kind + " " + rec.id;
  for (const auto& a : rec.attrs) header += " " + format_attr(a.key, a.value);
  emit(out, rec.trivia, header);
  for (const auto& l : rec.lines) emit(out, l.trivia, format_line(l.directive, l.text));
  for (const auto& s : rec.sections) {
    emit(out, s.trivia, "--- " + s.name);
    for (const auto& l : s.lines) {
      if (s.name == "diff" && (l.directive == "+" || l.directive == "-" || l.directive == "~")) {
        emit(out, l.trivia, l.text.empty() ? l.directive : l.directive + " " + l.text);
      } else {
        emit(out, l.trivia, format_line(l.directive, l.text));
      }
    }
  }
  for (const auto& c : rec.tail) out += (c.empty() ? "#" : "# " + c) + "\n";
  return out;
}

}  // namespace kk::core
