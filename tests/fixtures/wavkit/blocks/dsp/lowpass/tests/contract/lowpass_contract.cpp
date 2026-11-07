#include <doctest/doctest.h>

#include <vector>

#include "lowpass.hpp"

TEST_CASE("a constant signal passes through unchanged after settling") {
  auto lp = wk::make_lowpass();
  lp->set_cutoff(1000.0f, 48000.0f);
  std::vector<wk::Frame> f(4800, wk::Frame{0.5f, -0.25f});
  lp->process(f);
  CHECK(f.back().left == doctest::Approx(0.5f).epsilon(0.001));
  CHECK(f.back().right == doctest::Approx(-0.25f).epsilon(0.001));
}

TEST_CASE("output stays within the input range") {
  auto lp = wk::make_lowpass();
  lp->set_cutoff(200.0f, 48000.0f);
  std::vector<wk::Frame> f;
  for (int i = 0; i < 2000; ++i) f.push_back({(i / 50) % 2 == 0 ? 0.9f : -0.9f, 0.0f});
  lp->process(f);
  for (const auto& x : f) {
    CHECK(x.left <= 0.9f);
    CHECK(x.left >= -0.9f);
  }
}
