#include "image_decode.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cstring>

#include "i18n.hpp"

namespace kk::gui {

Pixels decode_image(const std::vector<std::uint8_t>& bytes, int max_side) {
  Pixels out;
  if (bytes.empty()) {
    out.error = tr("empty file");
    return out;
  }
  int w = 0;
  int h = 0;
  int comp = 0;
  stbi_uc* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
  if (data == nullptr) {
    out.error = std::string(tr("cannot read the image: ")) + stbi_failure_reason();
    return out;
  }
  out.full_w = w;
  out.full_h = h;
  // A box filter by a whole factor: a big photo becomes a texture of at most max_side.
  int f = std::max(1, (std::max(w, h) + max_side - 1) / max_side);
  out.w = std::max(1, w / f);
  out.h = std::max(1, h / f);
  out.rgba.resize(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h) * 4);
  if (f == 1) {
    std::memcpy(out.rgba.data(), data, out.rgba.size());
  } else {
    for (int y = 0; y < out.h; ++y) {
      for (int x = 0; x < out.w; ++x) {
        unsigned sum[4] = {0, 0, 0, 0};
        for (int dy = 0; dy < f; ++dy) {
          const stbi_uc* row = data + (static_cast<std::size_t>(y * f + dy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x * f)) * 4;
          for (int dx = 0; dx < f; ++dx) {
            for (int c = 0; c < 4; ++c) sum[c] += row[dx * 4 + c];
          }
        }
        auto* px = out.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(out.w) + static_cast<std::size_t>(x)) * 4;
        for (int c = 0; c < 4; ++c) px[c] = static_cast<std::uint8_t>(sum[c] / static_cast<unsigned>(f * f));
      }
    }
  }
  stbi_image_free(data);
  return out;
}

}  // namespace kk::gui
