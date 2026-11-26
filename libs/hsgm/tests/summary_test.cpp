#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "kk/hsgm/summary.hpp"
#include "support.hpp"

using namespace kk::hsgm;

TEST_CASE("summary: wavkit has one line per measurement and matches golden") {
  test::Wavkit w;
  auto s = summary(w.in);
  CHECK(std::count(s.begin(), s.end(), '\n') == 7);  // header, five measurements, where the assumptions are
  CHECK(s.find("propagation  transitive 0.32, direct 0.21, self 0.08") != std::string::npos);
  CHECK(kk::test::matches_golden("hsgm/wavkit_summary.txt", s));
}

TEST_CASE("summary: ends with where the assumptions are") {
  test::Wavkit w;
  auto s = summary(w.in);
  CHECK(s.find("\nassume ") != std::string::npos);
}

TEST_CASE("summary: a project without tests says its k is vacuous") {
  test::Make m;
  m.block("a", "leaf").block("b", "leaf").link("call", "a", "b", "", "f");
  CHECK(summary(m.in).find("k 2 (vacuous)") != std::string::npos);
}
