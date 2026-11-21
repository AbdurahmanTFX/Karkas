#include "kk/core/notes.hpp"

#include <array>
#include <filesystem>

#include "kk/core/markdown.hpp"
#include "kk/core/paths.hpp"
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
  std::string name(b.note_txt ? kOldNoteFileName : kNoteFileName);
  return b.dir.empty() ? name : b.dir + "/" + name;
}

std::optional<std::string> note_link_file(std::string_view from, std::string_view href) {
  if (href.empty() || md::is_url(href)) return std::nullopt;
  auto dest = std::string(href.substr(0, href.find_first_of("#?")));
  if (dest.empty()) return std::nullopt;  // an anchor in the same note
  dest = md::percent_decode(dest);
  auto ext = to_lower(std::filesystem::path(dest).extension().string());
  if (ext != ".md" && ext != ".markdown" && ext != ".txt") return std::nullopt;
  std::filesystem::path p = dest.starts_with('/') ? std::filesystem::path(dest.substr(1))
                                                  : std::filesystem::path(std::string(from)).parent_path() / dest;
  return p.lexically_normal().generic_string();
}

namespace {

// Folders that hold no files of the user's notes: hidden ones, build trees, downloaded sources.
bool skipped_folder(const std::string& name) {
  return name.starts_with(".") || is_build_dir_name(name) || name == "node_modules" || name == "third_party" || name == "_deps";
}

}  // namespace

MediaFinder::MediaFinder(std::filesystem::path root) : root_(std::move(root)) {}

std::optional<std::string> MediaFinder::find(std::string_view note_file, std::string_view src, bool wiki) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::path(std::string(note_file)).parent_path();
  auto here = [&](const fs::path& rel) -> std::optional<std::string> {
    auto n = rel.lexically_normal();
    if (n.empty() || !fs::is_regular_file(root_ / n, ec)) return std::nullopt;
    return n.generic_string();
  };
  if (!wiki) {
    if (src.empty() || md::is_url(src)) return std::nullopt;
    auto dest = md::percent_decode(src.substr(0, src.find_first_of("#?")));
    if (dest.empty()) return std::nullopt;
    return here(dest.starts_with('/') ? fs::path(dest.substr(1)) : dir / dest);
  }
  // ![[name]]: the note's folder, the project root, else the file of that name nearest to the note.
  const auto name = std::string(src);
  if (auto r = here(dir / name)) return r;
  if (auto r = here(fs::path(name))) return r;
  if (!by_name_) {
    by_name_.emplace();
    for (auto it = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      std::error_code fec;
      if (it->is_directory(fec)) {
        if (skipped_folder(it->path().filename().string())) it.disable_recursion_pending();
        continue;
      }
      if (md::is_image_file(it->path().filename().string())) {
        by_name_->emplace(it->path().filename().string(), rel_str(root_, it->path()));
      }
    }
  }
  auto [first, last] = by_name_->equal_range(fs::path(name).filename().string());
  std::optional<std::string> best;
  std::size_t best_common = 0;
  const auto d = dir.generic_string();
  for (auto it = first; it != last; ++it) {
    std::size_t common = 0;
    while (common < d.size() && common < it->second.size() && d[common] == it->second[common]) ++common;
    if (!best || common > best_common || (common == best_common && it->second < *best)) {
      best = it->second;
      best_common = common;
    }
  }
  return best;
}

namespace {

// Calls f(begin, end, name, inner, line) for each [[link]]: [begin, end) spans the brackets. An Obsidian
// embed of an image file, ![[file.png]], is no link (docs/adr/0041): it goes to `embed(begin, end)`.
template <typename F, typename E>
void each_ref(std::string_view text, F f, E embed) {
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
    auto inner = std::string(text.substr(i + 2, end - i - 2));
    auto name = std::string(trim(inner));
    // [[name|shown text]]: the part before '|' names the note.
    if (auto bar = name.find('|'); bar != std::string::npos) name = std::string(trim(std::string_view(name).substr(0, bar)));
    if (i > 0 && text[i - 1] == '!' && md::is_image_file(name)) {
      embed(i - 1, end + 2);
    } else if (!name.empty()) {
      f(i, end + 2, name, inner, line);
    }
    i = end + 1;
  }
}

template <typename F>
void each_ref(std::string_view text, F f) {
  each_ref(text, f, [](std::size_t, std::size_t) {});
}

}  // namespace

std::vector<NoteRef> parse_note_refs(std::string_view text) {
  std::vector<NoteRef> out;
  each_ref(text, [&](std::size_t, std::size_t, const std::string& name, const std::string&, int line) {
    out.push_back(NoteRef{.name = name, .line = line, .target = {}, .matches = 0});
  });
  return out;
}

std::string rewrite_note_refs(std::string_view text, const RefRewrite& f) {
  std::string out;
  std::size_t done = 0;
  each_ref(text, [&](std::size_t begin, std::size_t end, const std::string& name, const std::string& inner, int) {
    auto r = f(name, inner);
    if (!r) return;
    out.append(text.substr(done, begin - done));
    out += *r;
    done = end;
  });
  out.append(text.substr(done));
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

std::string lower_text(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    std::size_t start = i;
    char32_t c = next_cp(s, i);
    char32_t l = lower_cp(c);
    std::string piece;
    put_cp(piece, l);
    // Same length (ASCII, Cyrillic): the lowercase letter; anything else stays as written.
    if (c != 0xFFFD && piece.size() == i - start) {
      out += piece;
    } else {
      out.append(s.substr(start, i - start));
    }
  }
  return out;
}

namespace {

bool word_cp(char32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (c >= 0x0400 && c <= 0x04FF);
}

// The code point that ends just before byte `pos` (0 at the start).
char32_t cp_before(std::string_view s, std::size_t pos) {
  if (pos == 0) return 0;
  std::size_t b = pos - 1;
  while (b > 0 && (static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) --b;
  std::size_t i = b;
  return next_cp(s, i);
}

}  // namespace

std::vector<std::size_t> find_mentions(std::string_view text, std::string_view title) {
  std::vector<std::size_t> out;
  auto t = lower_text(trim(title));
  std::size_t letters = 0;
  for (std::size_t i = 0; i < t.size();) {
    next_cp(t, i);
    ++letters;
  }
  if (letters < 3) return out;
  auto low = lower_text(text);
  std::vector<std::pair<std::size_t, std::size_t>> links;
  each_ref(
      text, [&](std::size_t b, std::size_t e, const std::string&, const std::string&, int) { links.emplace_back(b, e); },
      [&](std::size_t b, std::size_t e) { links.emplace_back(b, e); });
  for (const auto& l : md::links(text)) links.emplace_back(l.begin, l.end);
  for (auto at = low.find(t); at != std::string::npos; at = low.find(t, at + 1)) {
    std::size_t end = at + t.size();
    std::size_t after = end;
    char32_t next = end < low.size() ? next_cp(low, after) : 0;
    if (word_cp(cp_before(low, at)) || word_cp(next)) continue;
    bool linked = false;
    for (const auto& [b, e] : links) linked = linked || (at >= b && at < e);
    if (!linked) out.push_back(at);
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
