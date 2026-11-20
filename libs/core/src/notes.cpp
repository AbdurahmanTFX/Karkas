#include "kk/core/notes.hpp"

#include <array>

#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"

namespace kk::core {

namespace {

// The next code point of UTF-8 text at `i` (advanced); U+FFFD for a broken byte.
char32_t next_cp(std::string_view s, std::size_t& i) {
  auto b = static_cast<unsigned char>(s[i++]);
  if (b < 0x80) return b;
  int n = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
  if (n < 0) return 0xFFFD;
  char32_t cp = b & (0x3F >> n);
  for (int k = 0; k < n; ++k) {
    if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
  }
  return cp;
}

void put_cp(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

char32_t lower_cp(char32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0x0410 && c <= 0x042F) return c + 32;   // А..Я
  if (c >= 0x0400 && c <= 0x040F) return c + 80;   // Ѐ..Џ (Ё, Є, І, Ї ...)
  if (c == 0x0490) return 0x0491;                  // Ґ
  return c;
}

// Latin letters of a lowercase Cyrillic letter (Russian, Ukrainian, Belarusian), nullptr for others.
const char* translit(char32_t c) {
  static constexpr std::array<const char*, 32> kRussian = {"a", "b", "v", "g", "d",  "e",    "zh", "z", "i", "y", "k",
                                                     "l", "m", "n", "o", "p",  "r",    "s",  "t", "u", "f", "kh",
                                                     "ts", "ch", "sh", "shch", "", "y", "",  "e", "yu", "ya"};
  if (c >= 0x0430 && c <= 0x044F) return kRussian[c - 0x0430];
  switch (c) {
    case 0x0451: return "yo";  // ё
    case 0x0454: return "ye";  // є
    case 0x0456: return "i";   // і
    case 0x0457: return "yi";  // ї
    case 0x045E: return "u";   // ў
    case 0x0491: return "g";   // ґ
    default: return nullptr;
  }
}

}  // namespace

bool is_notes_project(const Project& p) { return p.lang.value == "notes"; }

std::string note_file(const Block& b) {
  if (b.kind == BlockKind::Group) return {};
  if (b.external()) return b.file_list().front();
  return b.dir.empty() ? std::string(kNoteFileName) : b.dir + "/" + std::string(kNoteFileName);
}

std::vector<NoteRef> parse_note_refs(std::string_view text) {
  std::vector<NoteRef> out;
  int line = 1;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
      continue;
    }
    if (text[i] != '[' || i + 1 >= text.size() || text[i + 1] != '[') continue;
    auto end = text.find("]]", i + 2);
    auto nl = text.find('\n', i + 2);
    if (end == std::string_view::npos || (nl != std::string_view::npos && nl < end)) continue;
    auto name = std::string(trim(text.substr(i + 2, end - i - 2)));
    // [[name|shown text]]: the part before '|' names the note.
    if (auto bar = name.find('|'); bar != std::string::npos) name = std::string(trim(std::string_view(name).substr(0, bar)));
    if (!name.empty()) out.push_back(NoteRef{.name = name, .line = line, .target = {}, .matches = 0});
    i = end + 1;
  }
  return out;
}

std::string fold_title(std::string_view s) {
  std::string out;
  bool space = false;
  for (std::size_t i = 0; i < s.size();) {
    char32_t c = next_cp(s, i);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    put_cp(out, lower_cp(c));
  }
  return out;
}

std::vector<std::string> resolve_note(const Schema& s, std::string_view name) {
  auto n = std::string(trim(name));
  if (n.empty()) return {};
  // A block id, with dots or slashes.
  auto as_id = replace_all(n, "/", ".");
  if (s.find(as_id) != nullptr) return {as_id};
  auto folded = fold_title(n);
  std::vector<std::string> out;
  for (const auto& b : s.blocks) {
    if (fold_title(b.purpose.value) == folded || fold_title(last_segment(b.id)) == folded) out.push_back(b.id);
  }
  return out;
}

std::string note_link_name(const Schema& s, const std::string& id) {
  const Block* b = s.find(id);
  if (b == nullptr) return id;
  const auto& title = b->purpose.value;
  // A title with ']' or '|' cannot be written between [[ and ]].
  if (!title.empty() && title.find_first_of("[]|\n") == std::string::npos) {
    auto r = resolve_note(s, title);
    if (r.size() == 1 && r.front() == id) return title;
  }
  return id;
}

std::string ident_from_title(std::string_view title) {
  std::string out;
  bool sep = false;
  for (std::size_t i = 0; i < title.size();) {
    char32_t c = lower_cp(next_cp(title, i));
    std::string piece;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      piece = std::string(1, static_cast<char>(c));
    } else if (const char* t = translit(c)) {
      piece = t;
    } else {
      sep = true;  // spaces, punctuation, other scripts
      continue;
    }
    if (piece.empty()) continue;  // ъ, ь
    if (sep && !out.empty()) out += '_';
    sep = false;
    out += piece;
  }
  if (!out.empty() && !(out[0] >= 'a' && out[0] <= 'z')) out = "n_" + out;
  return out.empty() ? std::string("note") : out;
}

}  // namespace kk::core
