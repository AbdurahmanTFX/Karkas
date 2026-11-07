#include <doctest/doctest.h>

#include <vector>

#include "gain.hpp"

TEST_CASE("gain 1.0 leaves samples unchanged") {
  auto g = wk::make_gain();
  g->set_gain(1.0f);
  std::vector<wk::Frame> f;
  for (int i = -10; i <= 10; ++i) f.push_back({static_cast<float>(i) / 10.0f, static_cast<float>(-i) / 10.0f});
  auto copy = f;
  g->process(f);
  for (std::size_t i = 0; i < f.size(); ++i) {
    CHECK(f[i].left == copy[i].left);
    CHECK(f[i].right == copy[i].right);
  }
}

TEST_CASE("gain is applied exactly once per sample") {
  auto g = wk::make_gain();
  g->set_gain(0.5f);
  std::vector<wk::Frame> f(64, wk::Frame{0.8f, -0.4f});
  g->process(f);
  for (const auto& x : f) {
    CHECK(x.left == doctest::Approx(0.4f));
    CHECK(x.right == doctest::Approx(-0.2f));
  }
}
