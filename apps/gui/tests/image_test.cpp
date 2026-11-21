// Images of notes (docs/adr/0041): a PNG written by the GUI's own writer decodes to the same pixels; big
// images shrink; broken bytes give a reason, not a crash.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "image_decode.hpp"
#include "kk/core/paths.hpp"
#include "kk/test/support.hpp"
#include "png.hpp"

using namespace kk;

namespace {

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& p) {
  auto text = core::read_file(p);
  REQUIRE(text);
  return {text->begin(), text->end()};
}

}  // namespace

TEST_CASE("images: a PNG decodes to its pixels, a big one shrinks by a whole factor") {
  test::TempDir d{"gui_png"};
  std::vector<std::uint8_t> rgba;
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) rgba.insert(rgba.end(), {static_cast<std::uint8_t>(x * 60), static_cast<std::uint8_t>(y * 100), 7, 255});
  }
  REQUIRE(gui::write_png((d / "a.png").string(), 4, 3, rgba));
  auto px = gui::decode_image(read_bytes(d / "a.png"));
  CHECK(px.error.empty());
  CHECK(px.w == 4);
  CHECK(px.h == 3);
  CHECK(px.full_w == 4);
  CHECK(px.rgba == rgba);

  auto small = gui::decode_image(read_bytes(d / "a.png"), 2);  // at most 2 px: factor 2
  CHECK(small.w == 2);
  CHECK(small.h == 1);
  CHECK(small.full_w == 4);
  CHECK(small.full_h == 3);
  CHECK(small.rgba[0] == 30);   // (0 + 60) / 2: the box of 2×2 pixels
  CHECK(small.rgba[1] == 50);   // (0 + 0 + 100 + 100) / 4
  CHECK(small.rgba[3] == 255);
}

TEST_CASE("images: broken and empty bytes give a reason") {
  CHECK_FALSE(gui::decode_image({}).error.empty());
  auto bad = gui::decode_image({0x89, 'P', 'N', 'G', 1, 2, 3});
  CHECK_FALSE(bad.error.empty());
  CHECK(bad.rgba.empty());
}
