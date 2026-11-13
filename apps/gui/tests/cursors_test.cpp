#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "cursors.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

void put32(std::vector<unsigned char>& d, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) d.push_back(static_cast<unsigned char>(v >> (8 * i) & 0xff));
}

// An Xcursor file with one 2x1 image per nominal size; pixel 0 is opaque red, pixel 1 is
// half transparent white (premultiplied: 0x80808080).
void write_xcursor(const std::filesystem::path& file, const std::vector<int>& sizes) {
  std::vector<unsigned char> d;
  d.insert(d.end(), {'X', 'c', 'u', 'r'});
  put32(d, 16);
  put32(d, 0x10000);
  put32(d, static_cast<std::uint32_t>(sizes.size()));
  std::uint32_t pos = 16 + 12 * static_cast<std::uint32_t>(sizes.size());
  for (int s : sizes) {
    put32(d, 0xfffd0002);
    put32(d, static_cast<std::uint32_t>(s));
    put32(d, pos);
    pos += 36 + 2 * 4;
  }
  for (int s : sizes) {
    put32(d, 36);
    put32(d, 0xfffd0002);
    put32(d, static_cast<std::uint32_t>(s));
    put32(d, 1);
    put32(d, 2);  // width
    put32(d, 1);  // height
    put32(d, static_cast<std::uint32_t>(s == 24 ? 1 : 0));  // xhot tells the sizes apart
    put32(d, 0);
    put32(d, 0);
    put32(d, 0xffff0000);
    put32(d, 0x80808080);
  }
  std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

}  // namespace

TEST_CASE("cursors: the theme a default theme inherits by its display name is found, its image read straight") {
  test::TempDir d{"cursors"};
  auto icons = d.path() / "icons";
  std::filesystem::create_directories(icons / "default");
  std::filesystem::create_directories(icons / "Future-black-cursors" / "cursors");
  // What nwg-look writes: the theme's Name, with a space, not its folder.
  std::ofstream(icons / "default" / "index.theme") << "[Icon Theme]\nInherits=Future-black Cursors\n";
  std::ofstream(icons / "Future-black-cursors" / "index.theme") << "[Icon Theme]\nName=Future-black Cursors\n";
  write_xcursor(icons / "Future-black-cursors" / "cursors" / "pointer", {32, 24, 48});

  auto img = gui::load_theme_cursor({"pointer", "hand2"}, 24, "default", {icons});
  REQUIRE(img.has_value());
  CHECK(img->width == 2);
  CHECK(img->height == 1);
  CHECK(img->xhot == 1);  // the 24 px image of the three
  REQUIRE(img->rgba.size() == 8);
  CHECK(img->rgba[0] == 255);  // red, opaque
  CHECK(img->rgba[3] == 255);
  CHECK(img->rgba[4] == 255);  // white at half alpha: un-premultiplied
  CHECK(img->rgba[7] == 128);

  // A name the theme does not have, a theme that does not exist: nothing (GLFW's cursor stays).
  CHECK_FALSE(gui::load_theme_cursor({"no-such-cursor"}, 24, "default", {icons}).has_value());
  CHECK_FALSE(gui::load_theme_cursor({"pointer"}, 24, "Nope", {icons}).has_value());
  CHECK_FALSE(gui::read_xcursor(icons / "default" / "index.theme", 24).has_value());
}
