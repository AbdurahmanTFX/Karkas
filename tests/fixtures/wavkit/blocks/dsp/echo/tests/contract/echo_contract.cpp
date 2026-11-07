#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "echo.hpp"

TEST_CASE("feedback below 1.0 never makes the output grow without bound") {
  auto e = wk::make_echo();
  e->set_params(100, 0.9f, 0.5f);
  std::vector<wk::Frame> f(100000, wk::Frame{0.5f, 0.5f});
  e->process(f);
  float peak = 0.0f;
  for (const auto& x : f) peak = std::max(peak, std::fabs(x.left));
  CHECK(peak < 10.0f);
}

TEST_CASE("zero mix leaves samples unchanged") {
  auto e = wk::make_echo();
  e->set_params(10, 0.5f, 0.0f);
  std::vector<wk::Frame> f;
  for (int i = 0; i < 100; ++i) f.push_back({static_cast<float>(i) / 100.0f, 0.0f});
  auto copy = f;
  e->process(f);
  for (std::size_t i = 0; i < f.size(); ++i) CHECK(f[i].left == doctest::Approx(copy[i].left));
}
