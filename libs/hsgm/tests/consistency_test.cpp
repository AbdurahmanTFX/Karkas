#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "kk/hsgm/consistency.hpp"
#include "support.hpp"

using namespace kk::hsgm;
using kk::hsgm::test::Make;

using Sets = std::vector<std::string>;

TEST_CASE("consistency: each failed edge names the forms to regenerate") {
  Make m;
  for (const auto* id : {"a", "b", "c", "d", "e"}) m.block(id, "leaf");
  m.drift("a", "D04").drift("b", "D08");
  m.suite("c", "contract").result("c", {0, 1}, {}, {}, true, /*build_failed*/ true);
  m.suite("d", "contract").result("d", {0, 1});
  m.drift("e", "D05").suite("e", "contract").result("e", {0, 1});
  auto c = consistency(m.in);
  auto regen = [&](const std::string& id) {
    for (const auto& f : c.disagree) {
      if (f.block == id) return f.regenerate;
    }
    return Sets{};
  };
  CHECK(regen("a") == Sets{"{a}"});
  CHECK(regen("b") == Sets{"{t}"});
  CHECK(regen("c") == Sets{"{a}", "{t}", "{i}"});
  CHECK(regen("d") == Sets{"{t}", "{i}"});
  CHECK(regen("e") == Sets{"{a, t}", "{a, i}"});
}

TEST_CASE("consistency: test names match invariants with whitespace collapsed and semicolons read as commas") {
  Make m;
  m.block("a", "leaf").inv("a", "x stays in [0, n]; y holds").test_case("a", "x  stays in [0, n], y holds");
  CHECK(consistency(m.in).disagree.empty());
  Make u;
  u.block("a", "leaf").inv("a", "never blocks");
  auto c = consistency(u.in);
  REQUIRE(c.disagree.size() == 1);
  CHECK(c.disagree[0].untested == Sets{"never blocks"});
}

// Review focus: results older than the code.
TEST_CASE("consistency: stale results do not fail an edge") {
  Make m;
  m.block("a", "leaf").suite("a", "contract").result("a", {0, 1}, {}, {}, /*fresh*/ false);
  CHECK(consistency(m.in).disagree.empty());
}

TEST_CASE("consistency: wavkit has stream.ring failing t-i and matches golden") {
  test::Wavkit w;
  auto c = consistency(w.in);
  CHECK(c.checked == 10);
  REQUIRE(c.disagree.size() == 1);
  CHECK(c.disagree[0].block == "stream.ring");
  CHECK(c.disagree[0].regenerate == Sets{"{t}", "{i}"});
  CHECK(kk::test::matches_golden("hsgm/wavkit_consistency.txt", to_text(c)));
}

TEST_CASE("consistency: imported leaves and entries are not asked for contract tests") {
  Make m;
  m.block("a", "leaf").external("a").inv("a", "keeps order").block("e", "entry").inv("e", "starts once");
  CHECK(consistency(m.in).disagree.empty());
}
