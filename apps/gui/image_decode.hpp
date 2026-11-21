#pragma once
// Image files to pixels for the notes' images (docs/adr/0041): stb_image, PNG, JPEG, GIF (its first frame)
// and BMP, from bytes already read. No OpenGL here (unit tests use it).
#include <cstdint>
#include <string>
#include <vector>

namespace kk::gui {

struct Pixels {
  int w = 0, h = 0;            // of `rgba`
  int full_w = 0, full_h = 0;  // of the file
  std::vector<std::uint8_t> rgba;
  std::string error;  // "" when decoded
};

// RGBA, top row first, shrunk by a whole factor (box filter) to at most `max_side` px on the long side.
Pixels decode_image(const std::vector<std::uint8_t>& bytes, int max_side = 2048);

}  // namespace kk::gui
