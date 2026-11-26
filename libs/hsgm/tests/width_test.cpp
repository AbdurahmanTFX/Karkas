#include <doctest/doctest.h>

#include <string>
#include <utility>

#include "kk/hsgm/width.hpp"
#include "support.hpp"

using namespace kk::hsgm;
using kk::hsgm::test::Make;

namespace {

// Groups of 2, 4 and 8 leaves.
Make widths_project() {
  Make m;
  for (auto [g, n] : {std::pair{"g2", 2}, std::pair{"g4", 4}, std::pair{"g8", 8}}) {
    m.block(g, "group");
    for (int i = 0; i < n; ++i) m.block(std::string(g) + ".l" + std::to_string(i), "leaf");
  }
  return m;
}

}  // namespace

TEST_CASE("width: runs made from known c0 and c1 give the known best width") {
  auto m = widths_project();
  for (auto [g, b] : {std::pair{"g2", 2}, std::pair{"g4", 4}, std::pair{"g8", 8}}) {
    for (int k = 0; k < 2; ++k) m.run("navigator", g, 0.01 + 0.002 * b);
  }
  auto w = width(m.in);
  CHECK(w.enough);
  CHECK(w.unit == "usd");
  CHECK(w.c0 == doctest::Approx(0.01));
  CHECK(w.c1 == doctest::Approx(0.002));
  CHECK(w.best == doctest::Approx(6.14).epsilon(0.01));  // ln b = 1 + 5 / b
  CHECK(w.ratio == doctest::Approx(1.022).epsilon(0.005));
}

TEST_CASE("width: too few runs or one width is not enough data") {
  auto m = widths_project();
  for (int k = 0; k < 5; ++k) m.run("navigator", "g4", 0.02);
  CHECK_FALSE(width(m.in).enough);
}

// Review focus: runs that must not enter the fit.
TEST_CASE("width: session-cost runs and runs on unknown groups are left out") {
  auto m = widths_project();
  m.run("navigator", "gone", 0.5).run("navigator", "g2", 0.5, 0, /*session_cost*/ true).run("worker", "g2", 0.5);
  CHECK(width(m.in).runs == 0);
}

TEST_CASE("width: runs without a cost are fitted in tokens") {
  auto m = widths_project();
  for (auto [g, b] : {std::pair{"g2", 2}, std::pair{"g8", 8}}) {
    for (int k = 0; k < 3; ++k) m.run("navigator", g, 0, 1000 + 200 * b);
  }
  CHECK(width(m.in).unit == "tokens");
}

// Review focus: a project without groups.
TEST_CASE("width: a flat project has no groups and not enough data") {
  Make m;
  m.block("a", "leaf");
  auto w = width(m.in);
  CHECK(w.mean == 0);
  CHECK_FALSE(w.enough);
}

TEST_CASE("width: wavkit has no runs and matches golden") {
  test::Wavkit w;
  CHECK(kk::test::matches_golden("hsgm/wavkit_width.txt", to_text(width(w.in))));
}
