#include "cursors.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>

namespace kk::gui {

namespace fs = std::filesystem;

namespace {

std::uint32_t u32(const std::vector<unsigned char>& d, std::size_t at) {
  if (at + 4 > d.size()) return 0;
  return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8 | static_cast<std::uint32_t>(d[at + 2]) << 16 |
         static_cast<std::uint32_t>(d[at + 3]) << 24;
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// A key of the [Icon Theme] section of an index.theme ("" when absent).
std::string theme_key(const fs::path& index, const std::string& key) {
  std::ifstream in(index);
  std::string line;
  while (std::getline(in, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
  }
  return {};
}

// Inherited names: separated by ',' or ';' — a name may contain spaces.
std::vector<std::string> split_inherits(const std::string& v) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : v) {
    if (c == ',' || c == ';') {
      if (!trim(cur).empty()) out.push_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!trim(cur).empty()) out.push_back(trim(cur));
  return out;
}

// The folders of the theme called `name`: a folder with that name, else one whose index.theme
// has Name=<name>, or whose folder name equals it with spaces as '-' or '_' (case ignored).
std::vector<fs::path> theme_dirs(const std::string& name, const std::vector<fs::path>& search) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& base : search) {
    if (fs::is_directory(base / name, ec)) out.push_back(base / name);
  }
  if (!out.empty()) return out;
  std::string want = lower(name);
  std::string dashed = want;
  std::ranges::replace(dashed, ' ', '-');
  std::string under = want;
  std::ranges::replace(under, ' ', '_');
  for (const auto& base : search) {
    if (!fs::is_directory(base, ec)) continue;
    for (const auto& e : fs::directory_iterator(base, ec)) {
      if (!e.is_directory(ec)) continue;
      std::string dir = lower(e.path().filename().string());
      bool same = dir == dashed || dir == under || lower(theme_key(e.path() / "index.theme", "Name")) == want;
      if (same) out.push_back(e.path());
    }
  }
  return out;
}

std::optional<CursorImage> find_in_theme(const std::string& theme, const std::vector<std::string>& names, int size,
                                         const std::vector<fs::path>& search, std::set<std::string>& seen, int depth) {
  if (depth > 8 || !seen.insert(lower(theme)).second) return std::nullopt;
  auto dirs = theme_dirs(theme, search);
  std::error_code ec;
  for (const auto& name : names) {
    for (const auto& d : dirs) {
      fs::path f = d / "cursors" / name;
      if (fs::exists(f, ec)) {
        if (auto img = read_xcursor(f, size)) return img;
      }
    }
  }
  for (const auto& d : dirs) {
    for (const auto& parent : split_inherits(theme_key(d / "index.theme", "Inherits"))) {
      if (auto img = find_in_theme(parent, names, size, search, seen, depth + 1)) return img;
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<CursorImage> read_xcursor(const fs::path& file, int size) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (d.size() < 16 || d[0] != 'X' || d[1] != 'c' || d[2] != 'u' || d[3] != 'r') return std::nullopt;
  std::uint32_t header = u32(d, 4);
  std::uint32_t ntoc = u32(d, 12);
  constexpr std::uint32_t kImage = 0xfffd0002;
  // The nominal size nearest to the one asked for; its first image (animations have several).
  std::uint32_t best_pos = 0;
  int best_diff = 1 << 30;
  for (std::uint32_t i = 0; i < ntoc && i < 4096; ++i) {
    std::size_t at = header + static_cast<std::size_t>(i) * 12;
    if (u32(d, at) != kImage) continue;
    int nominal = static_cast<int>(u32(d, at + 4));
    int diff = std::abs(nominal - size);
    if (diff < best_diff) {
      best_diff = diff;
      best_pos = u32(d, at + 8);
    }
  }
  if (best_pos == 0) return std::nullopt;
  std::size_t at = best_pos;
  if (u32(d, at + 4) != kImage) return std::nullopt;
  CursorImage img;
  img.width = static_cast<int>(u32(d, at + 16));
  img.height = static_cast<int>(u32(d, at + 20));
  img.xhot = static_cast<int>(u32(d, at + 24));
  img.yhot = static_cast<int>(u32(d, at + 28));
  if (img.width <= 0 || img.height <= 0 || img.width > 1024 || img.height > 1024) return std::nullopt;
  std::size_t px = at + u32(d, at);  // pixels follow the image header
  std::size_t n = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
  if (px + n * 4 > d.size()) return std::nullopt;
  img.rgba.resize(n * 4);
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t argb = u32(d, px + i * 4);
    unsigned a = argb >> 24 & 0xff;
    unsigned r = argb >> 16 & 0xff;
    unsigned g = argb >> 8 & 0xff;
    unsigned b = argb & 0xff;
    if (a > 0 && a < 255) {  // Xcursor pixels are premultiplied
      r = std::min(255u, r * 255 / a);
      g = std::min(255u, g * 255 / a);
      b = std::min(255u, b * 255 / a);
    }
    img.rgba[i * 4] = static_cast<unsigned char>(r);
    img.rgba[i * 4 + 1] = static_cast<unsigned char>(g);
    img.rgba[i * 4 + 2] = static_cast<unsigned char>(b);
    img.rgba[i * 4 + 3] = static_cast<unsigned char>(a);
  }
  img.xhot = std::clamp(img.xhot, 0, img.width - 1);
  img.yhot = std::clamp(img.yhot, 0, img.height - 1);
  return img;
}

std::vector<fs::path> cursor_search_path() {
  std::vector<fs::path> out;
  const char* home = std::getenv("HOME");
  auto expand = [&](std::string p) -> fs::path {
    if (p.starts_with("~/") && home != nullptr) return fs::path(home) / p.substr(2);
    return p;
  };
  if (const char* xp = std::getenv("XCURSOR_PATH"); xp != nullptr && *xp != '\0') {
    std::string all = xp;
    std::size_t pos = 0;
    while (pos <= all.size()) {
      auto colon = all.find(':', pos);
      std::string part = all.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
      if (!part.empty()) out.push_back(expand(part));
      if (colon == std::string::npos) break;
      pos = colon + 1;
    }
    return out;
  }
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
    out.push_back(fs::path(xdg) / "icons");
  } else {
    out.push_back(expand("~/.local/share/icons"));
  }
  out.push_back(expand("~/.icons"));
  out.emplace_back("/usr/share/icons");
  out.emplace_back("/usr/share/pixmaps");
  return out;
}

std::optional<CursorImage> load_theme_cursor(const std::vector<std::string>& names, int size, const std::string& theme,
                                             const std::vector<fs::path>& search) {
  std::vector<fs::path> path = search.empty() ? cursor_search_path() : search;
  std::string start = theme;
  if (start.empty()) {
    const char* t = std::getenv("XCURSOR_THEME");
    start = t != nullptr && *t != '\0' ? t : "default";
  }
  std::set<std::string> seen;
  return find_in_theme(start, names, size, path, seen, 0);
}

}  // namespace kk::gui
