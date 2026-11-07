#pragma once
// Minimal PNG writer (RGBA, stored deflate blocks) for GUI screenshots (--screenshot).
#include <cstdint>
#include <string>
#include <vector>

namespace kk::gui {
bool write_png(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgba_top_down);
}
