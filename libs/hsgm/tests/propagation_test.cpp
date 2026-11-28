#include <doctest/doctest.h>

#include <string>
#include <utility>

#include "kk/hsgm/propagation.hpp"
#include "support.hpp"

using namespace kk::hsgm;
using kk::hsgm::test::Make;

TEST_CASE("propagation: the example of MacCormack et al. gives 15 of 36") {
  // Figure 2 of MacCormack, Baldwin, Rusnak 2012: A uses B and C, B uses D, C uses E, E uses F.
  Make m;
  for (const auto* id : {"a", "b", "c", "d", "e", "f"}) m.block(id, "leaf");
  m.link("call", "a", "b", "", "f").link("call", "a", "c", "", "f").link("call", "b", "d", "", "f");
  m.link("call", "c", "e", "", "f").link("call", "e", "f", "", "f");
  auto p = propagation(m.in);
  CHECK(p.n == 6);
  CHECK(p.transitive == 15);
  CHECK(p.direct == 11);
  REQUIRE_FALSE(p.reach.empty());
  CHECK(p.reach[0] == std::pair<std::string, int>{"f", 4});
}

TEST_CASE("propagation: data makes the consumer depend on the provider, state works both ways") {
  Make m;
  m.block("c", "leaf").block("p", "leaf").block("x", "leaf").block("y", "leaf");
  m.link("data", "p", "c", "out", "in").link("state", "x", "y", "s");
  auto p = propagation(m.in);
  auto r = [&](const std::string& id) {
    for (const auto& [b, n] : p.reach) {
      if (b == id) return n;
    }
    return -1;
  };
  CHECK(r("p") == 2);
  CHECK(r("c") == 1);
  CHECK(r("x") == 2);
  CHECK(r("y") == 2);
}

TEST_CASE("propagation: wavkit is 54, 35 and 13 of 169 and matches golden") {
  test::Wavkit w;
  auto p = propagation(w.in);
  CHECK(p.n == 13);
  CHECK(p.transitive == 54);
  CHECK(p.direct == 35);
  CHECK(kk::test::matches_golden("hsgm/wavkit_propagation.txt", to_text(p)));
}
