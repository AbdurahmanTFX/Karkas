#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "kk/hsgm/summary.hpp"
#include "support.hpp"

using namespace kk::hsgm;

TEST_CASE("summary: wavkit has one line per measurement and matches golden") {
  test::Wavkit w;
  auto s = summary(w.in);
  CHECK(std::count(s.begin(), s.end(), '\n') == 6);
  CHECK(s.find("propagation  transitive 0.32, direct 0.21, self 0.08") != std::string::npos);
  CHECK(kk::test::matches_golden("hsgm/wavkit_summary.txt", s));
}
