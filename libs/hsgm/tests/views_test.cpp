#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "kk/hsgm/views.hpp"
#include "support.hpp"

using namespace kk::hsgm;
using kk::hsgm::test::Make;

using Cycles = std::vector<std::vector<std::string>>;

TEST_CASE("views: a source and a sink in one group make false pairs and a false cycle") {
  Make m;
  m.block("p", "group").block("p.src", "leaf").block("p.sink", "leaf");
  m.block("q", "group").block("q.x", "leaf").block("r", "group").block("r.y", "leaf");
  m.link("data", "p.src", "q.x").link("data", "q.x", "r.y").link("data", "r.y", "p.sink");
  auto v = views(m.in);
  REQUIRE(v.levels.size() == 1);
  CHECK(v.levels[0].pairs == 6);
  CHECK(v.levels[0].true_pairs == 5);
  CHECK(v.levels[0].false_pairs == std::vector<std::string>{"r -> q"});
  CHECK(v.levels[0].false_cycles == Cycles{{"p", "q", "r"}});
}

TEST_CASE("views: a real cycle between two groups stays real") {
  Make m;
  m.block("g", "group").block("g.a", "leaf").block("h", "group").block("h.c", "leaf");
  m.link("call", "g.a", "h.c", "", "f").link("call", "h.c", "g.a", "", "f");
  auto v = views(m.in);
  REQUIRE(v.levels.size() == 1);
  CHECK(v.levels[0].real_cycles.size() == 1);
  CHECK(v.levels[0].false_cycles.empty());
}

TEST_CASE("views: a grouping that adds nothing has precision 1") {
  Make m;
  m.block("g", "group").block("g.a", "leaf").block("h", "group").block("h.b", "leaf").link("data", "g.a", "h.b");
  auto v = views(m.in);
  REQUIRE(v.levels.size() == 1);
  CHECK(v.levels[0].pairs == 1);
  CHECK(v.levels[0].true_pairs == 1);
  CHECK(v.levels[0].false_cycles.empty());
}

// Review focus: a project without groups.
TEST_CASE("views: a flat project has nothing lifted") {
  Make m;
  m.block("a", "leaf").block("b", "leaf").link("data", "a", "b");
  auto v = views(m.in);
  CHECK(v.levels.empty());
  CHECK(to_text(v).find("nothing lifted") != std::string::npos);
}

TEST_CASE("views: wavkit level 0 is 9 of 12 with one false cycle and matches golden") {
  test::Wavkit w;
  auto v = views(w.in);
  REQUIRE(v.levels.size() == 1);
  CHECK(v.levels[0].true_pairs == 9);
  CHECK(v.levels[0].pairs == 12);
  CHECK(v.levels[0].false_cycles == Cycles{{"dsp", "io", "mix", "stream"}});
  CHECK(kk::test::matches_golden("hsgm/wavkit_views.txt", to_text(v)));
}

TEST_CASE("views: a real cycle away from the first node of the lifted cycle stays real") {
  Make m;
  m.block("p", "group").block("p.a", "leaf").block("p.b", "leaf");
  m.block("q", "group").block("q.b", "leaf").block("r", "group").block("r.d", "leaf");
  m.link("data", "p.a", "q.b").link("call", "q.b", "r.d", "", "f").link("call", "r.d", "q.b", "", "g").link("data", "r.d", "p.b");
  auto v = views(m.in);
  REQUIRE(v.levels.size() == 1);
  CHECK(v.levels[0].real_cycles == Cycles{{"p", "q", "r"}});
  CHECK(v.levels[0].false_cycles.empty());
}
