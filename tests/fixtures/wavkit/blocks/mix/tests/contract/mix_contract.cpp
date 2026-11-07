#include <doctest/doctest.h>

#include <vector>

#include "mix.hpp"

TEST_CASE("output samples stay within [-1, 1]") {
  auto m = wk::make_mix();
  std::vector<std::vector<wk::Frame>> tracks(4, std::vector<wk::Frame>(100, wk::Frame{0.9f, -0.9f}));
  std::vector<wk::Frame> out;
  m->mix(tracks, out);
  REQUIRE(out.size() == 100);
  for (const auto& f : out) {
    CHECK(f.left <= 1.0f);
    CHECK(f.right >= -1.0f);
  }
}

TEST_CASE("a single track with gain 1.0 passes through unchanged") {
  auto m = wk::make_mix();
  m->set_track_gain(0, 1.0f);
  std::vector<std::vector<wk::Frame>> tracks = {{{0.25f, -0.5f}, {0.1f, 0.2f}}};
  std::vector<wk::Frame> out;
  m->mix(tracks, out);
  CHECK(out[0].left == 0.25f);
  CHECK(out[1].right == 0.2f);
}
