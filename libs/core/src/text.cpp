#include "kk/core/text.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <chrono>

#include "kk/core/error.hpp"

namespace kk {

std::string Error::str() const {
  std::string out;
  if (!loc.file.empty()) {
    out += loc.file;
    if (loc.line > 0) {
      out += fmt::format(":{}", loc.line);
      if (loc.col > 0) out += fmt::format(":{}", loc.col);
    }
    out += ": ";
  }
  out += "error";
  if (!code.empty()) out += " " + code;
  out += ": " + message;
  return out;
}

}  // namespace kk

namespace kk::core {

namespace {
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_lower_alpha(char c) { return c >= 'a' && c <= 'z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

std::string_view trim(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && is_space(s[b])) ++b;
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> split(std::string_view s, char sep, bool skip_empty) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    std::size_t pos = s.find(sep, start);
    if (pos == std::string_view::npos) pos = s.size();
    auto part = trim(s.substr(start, pos - start));
    if (!part.empty() || !skip_empty) out.emplace_back(part);
    start = pos + 1;
  }
  return out;
}

std::vector<std::string> split_ws(std::string_view s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && is_space(s[i])) ++i;
    std::size_t j = i;
    while (j < s.size() && !is_space(s[j])) ++j;
    if (j > i) out.emplace_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += sep;
    out += parts[i];
  }
  return out;
}

std::string pad_right(std::string_view s, std::size_t width) {
  std::string out(s);
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  } else {
    out.push_back(' ');
  }
  return out;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
  if (from.empty()) return s;
  std::size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

bool contains(std::string_view s, std::string_view needle) {
  return s.find(needle) != std::string_view::npos;
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool is_ident(std::string_view s) {
  if (s.empty() || !is_lower_alpha(s[0])) return false;
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return is_lower_alpha(c) || is_digit(c) || c == '_'; });
}

bool is_block_path(std::string_view s) {
  if (s.empty()) return false;
  std::size_t start = 0;
  while (true) {
    std::size_t dot = s.find('.', start);
    auto part = s.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    if (!is_ident(part)) return false;
    if (dot == std::string_view::npos) return true;
    start = dot + 1;
  }
}

bool is_record_id(std::string_view s) {
  if (s.size() < 3 || (s[0] != 'T' && s[0] != 'C') || s[1] != '-') return false;
  return std::all_of(s.begin() + 2, s.end(), is_digit);
}

bool needs_quotes(std::string_view s) {
  if (s.empty()) return true;
  for (char c : s) {
    if (is_space(c) || c == '"' || c == '#' || c == '\\') return true;
  }
  return false;
}

std::string quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::size_t estimate_tokens(std::string_view text) {
  // Heuristic close to BPE tokenizers on English/code: a letter run costs one token
  // per started 4 characters, digits one per 3, every other non-space char one token,
  // non-ASCII bytes one token per 2 bytes. Whitespace is free (merged with the next
  // word) except newlines.
  std::size_t tokens = 0;
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    auto c = static_cast<unsigned char>(text[i]);
    if (std::isalpha(c) != 0 || c == '_') {
      std::size_t j = i;
      while (j < n && (std::isalpha(static_cast<unsigned char>(text[j])) != 0 || text[j] == '_')) ++j;
      tokens += (j - i + 3) / 4;
      i = j;
    } else if (std::isdigit(c) != 0) {
      std::size_t j = i;
      while (j < n && std::isdigit(static_cast<unsigned char>(text[j])) != 0) ++j;
      tokens += (j - i + 2) / 3;
      i = j;
    } else if (c == '\n') {
      ++tokens;
      ++i;
    } else if (c == ' ' || c == '\t' || c == '\r') {
      std::size_t j = i;
      while (j < n && (text[j] == ' ' || text[j] == '\t')) ++j;
      if (j - i > 4) ++tokens;  // long space runs (column alignment)
      i = j;
    } else if (c >= 0x80) {
      std::size_t j = i;
      while (j < n && static_cast<unsigned char>(text[j]) >= 0x80) ++j;
      tokens += (j - i + 1) / 2;
      i = j;
    } else {
      ++tokens;
      ++i;
    }
  }
  return tokens;
}

std::uint64_t fnv1a64(std::string_view s) {
  std::uint64_t h = 14695981039346656037ULL;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

std::string hex64(std::uint64_t v) { return fmt::format("{:016x}", v); }

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace kk::core
