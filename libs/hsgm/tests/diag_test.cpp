#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "kk/hsgm/diag.hpp"
#include "support.hpp"

using namespace kk::hsgm;
using kk::hsgm::test::Make;

TEST_CASE("diag: every block with its own suite is identifiable alone and k is 2") {
  Make m;
  m.block("a", "leaf").block("b", "leaf").block("c", "leaf").suite("a", "contract").suite("b", "contract").suite("c", "contract");
  auto d = diag(m.in);
  CHECK(d.items.size() == 3);
  for (const auto& i : d.items) {
    CHECK(i.alone);
    CHECK(i.cls == 0);
  }
  CHECK(d.k == 2);
}

TEST_CASE("diag: items with the same column form one class") {
  Make m;
  m.block("g", "group").block("g.x", "leaf").block("g.y", "leaf").link("call", "g.x", "g.y", "", "run").suite("g", "integration");
  auto d = diag(m.in);
  CHECK(d.items.size() == 3);  // g.x, g.y, the call
  for (const auto& i : d.items) {
    CHECK(i.cls == 1);
    CHECK_FALSE(i.alone);
  }
  CHECK(d.k == 0);
  CHECK(d.witness == "g.x inside g.y");
}

TEST_CASE("diag: a block without suites is undetectable and a shared block in every suite is flagged") {
  Make m;
  m.block("a", "leaf").block("b", "leaf").block("c", "leaf").block("s", "shared");
  m.link("use", "a", "s").link("use", "b", "s").suite("a", "contract").suite("b", "contract");
  auto d = diag(m.in);  // items sorted: a, b, c, s
  REQUIRE(d.items.size() == 4);
  CHECK(d.items[2].undetectable);
  CHECK(d.items[3].everywhere);
  CHECK(d.items[3].alone);
}

TEST_CASE("diag: max_disjunct finds k 1 and its witness") {
  std::pair<int, std::vector<int>> w;
  CHECK(max_disjunct({{1, 2}, {1, 3}, {2, 3}}, 2, &w) == 1);
  CHECK(w.first == 0);
  CHECK(w.second == std::vector<int>{1, 2});
}

TEST_CASE("diag: COMP and DD read the fresh outcomes") {
  Make m;
  m.block("g", "group").block("g.a", "leaf").block("g.b", "leaf").block("c", "leaf");
  m.link("data", "g.a", "g.b", "out", "in");
  m.suite("g.a", "contract").suite("g.b", "contract").suite("c", "contract").suite("g", "integration");
  m.result("g.a", {1, 1}).result("g.b", {1, 1}).result("c", {0, 1}).result("g", {}, {}, {0, 1});
  auto d = diag(m.in);
  CHECK(d.failed == std::vector<std::string>{"contract:c", "integration:g"});
  CHECK(d.surely == std::vector<std::string>{"c", "data g.a:out -> g.b:in"});
}

// Review focus: inputs the measurements meet on real projects.
TEST_CASE("diag: a project without test files has every item undetectable and a vacuous k") {
  Make m;
  m.block("a", "leaf").block("b", "leaf").link("call", "a", "b", "", "f");
  auto d = diag(m.in);
  for (const auto& i : d.items) CHECK(i.undetectable);
  CHECK(d.suites.empty());
  CHECK(d.k == 2);
  CHECK(d.passed.empty());
}

TEST_CASE("diag: stale results are unknown, not failed") {
  Make m;
  m.block("a", "leaf").suite("a", "contract").result("a", {0, 1}, {}, {}, /*fresh*/ false);
  auto d = diag(m.in);
  CHECK(d.failed.empty());
  CHECK(d.unknown == std::vector<std::string>{"contract:a"});
}

TEST_CASE("diag: a link whose end is not a code block is no item") {
  Make m;
  m.block("g", "group").block("a", "leaf").link("call", "a", "g", "", "f").link("call", "a", "gone", "", "f");
  CHECK(diag(m.in).items.size() == 1);
}

TEST_CASE("input: TEST_CASE names are read in order and unescaped") {
  CHECK(test_case_names("TEST_CASE(\"x \\\"q\\\"\") {}\nTEST_CASE( \"y\" ) {}") == std::vector<std::string>{"x \"q\"", "y"});
}

TEST_CASE("diag: wavkit counts follow the research and match golden") {
  test::Wavkit w;
  auto d = diag(w.in);
  int blocks_alone = 0, links_alone = 0, links_class = 0, links_none = 0;
  for (const auto& i : d.items) {
    if (!i.link && i.alone) ++blocks_alone;
    if (i.link) {
      links_alone += i.alone ? 1 : 0;
      links_class += i.cls > 0 ? 1 : 0;
      links_none += i.undetectable ? 1 : 0;
    }
  }
  CHECK(d.suites.size() == 11);
  CHECK(blocks_alone == 11);
  CHECK(links_alone == 0);
  CHECK(links_class == 3);
  CHECK(links_none == 5);
  CHECK(d.k == 0);
  CHECK(d.dominated == 5);
  CHECK(d.witness == "stream.pump inside shared.probe");
  CHECK(d.surely == std::vector<std::string>{"stream.ring"});
  CHECK(kk::test::matches_golden("hsgm/wavkit_diag.txt", format(d)));
}
