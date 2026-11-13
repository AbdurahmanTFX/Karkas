#pragma once
// Mouse cursors from the desktop's cursor theme (ADR 0021). GLFW asks the Wayland cursor
// library for the theme "default"; when its index.theme inherits a theme by its display name
// ("Future-black Cursors", as nwg-look writes it, for the folder Future-black-cursors) that
// library finds nothing and falls back to the old X11 set, whose hand points to the upper
// right. Here the theme is found the way a person means it — XCURSOR_THEME, else "default" and
// what it inherits, a name matched against folder names and the themes' own Name= — and its
// Xcursor images are read directly.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kk::gui {

struct CursorImage {
  int width = 0, height = 0;
  int xhot = 0, yhot = 0;
  std::vector<unsigned char> rgba;  // straight (not premultiplied) RGBA, row by row
};

// One Xcursor file: the first image of the nominal size nearest to `size`.
std::optional<CursorImage> read_xcursor(const std::filesystem::path& file, int size);

// The folders a theme name may mean, in the order the cursor libraries look (XCURSOR_PATH or
// ~/.local/share/icons, ~/.icons, /usr/share/icons, /usr/share/pixmaps).
std::vector<std::filesystem::path> cursor_search_path();

// The cursor `names` (the first that exists) of the theme `theme` ("" = XCURSOR_THEME, else
// "default"), following Inherits=; `search` overrides the search path (tests).
std::optional<CursorImage> load_theme_cursor(const std::vector<std::string>& names, int size, const std::string& theme = {},
                                             const std::vector<std::filesystem::path>& search = {});

}  // namespace kk::gui
