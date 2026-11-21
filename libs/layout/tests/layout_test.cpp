#include <doctest/doctest.h>

#include <map>

#include "kk/layout/layout.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {
bool overlap(const layout::Pos& a, const layout::Node& na, const layout::Pos& b, const layout::Node& nb) {
  return a.x < b.x + nb.w && b.x < a.x + na.w && a.y < b.y + nb.h && b.y < a.y + na.h;
}
layout::Measure fixed() {
  return [](const index::BlockRow&) { return std::make_pair(160.0, 48.0); };
}
}  // namespace

TEST_CASE("layout: data flows left to right, no overlaps, deterministic") {
  layout::Graph g;
  for (const char* id : {"a", "b", "c", "d", "e"}) g.nodes.push_back({id, 120, 40});
  g.edges = {{"a", "b", "data"}, {"b", "c", "data"}, {"a", "d", "data"}, {"d", "c", "data"}, {"c", "e", "data"}, {"e", "a", "event"}};
  layout::SugiyamaEngine eng;
  auto l = eng.layout(g);
  REQUIRE(l);
  CHECK(l->pos["a"].x < l->pos["b"].x);
  CHECK(l->pos["b"].x < l->pos["c"].x);
  CHECK(l->pos["c"].x < l->pos["e"].x);  // the cycle e->a is broken
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    for (std::size_t j = i + 1; j < g.nodes.size(); ++j) {
      CHECK_FALSE(overlap(l->pos[g.nodes[i].id], g.nodes[i], l->pos[g.nodes[j].id], g.nodes[j]));
    }
  }
  auto again = eng.layout(g);
  CHECK(again->pos["d"].y == l->pos["d"].y);
  CHECK(l->width > 0);
  CHECK(l->height > 0);
}

TEST_CASE("layout: level graphs of wavkit, cache and manual positions") {
  test::FixtureCopy p("wavkit");
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  REQUIRE((*idx)->update());
  auto snap = (*idx)->snapshot();
  auto g = layout::level_graph(snap, "", false, fixed());
  CHECK(g.nodes.size() == 5);  // shared hidden (3.7)
  CHECK(g.edges.size() == 4);
  auto with_use = layout::level_graph(snap, "", true, fixed());
  CHECK(with_use.nodes.size() == 6);
  layout::SugiyamaEngine eng;
  layout::LayoutStore store(p.path());
  auto l = store.get("root", g, eng);
  CHECK(l.pos["io"].x < l.pos["stream"].x);
  CHECK(l.pos["stream"].x < l.pos["dsp"].x);
  CHECK(std::filesystem::exists(p / ".karkas/cache/layout"));
  REQUIRE(store.save_manual("root", {{"mix", {900, 10, 300, 120}}, {"io", {5, 5}}}));
  auto m = store.get("root", g, eng);
  CHECK(m.pos["mix"].x == 900);
  auto manual = store.manual("root");
  CHECK(manual["mix"].w == 300);
  CHECK(manual["mix"].h == 120);
  CHECK(manual["io"].w == 0);  // no manual size
  CHECK(layout::graph_hash(g) == layout::graph_hash(layout::level_graph(snap, "", false, fixed())));
  auto dsp = layout::level_graph(snap, "dsp", false, fixed());
  CHECK(dsp.nodes.size() == 4);
  CHECK(dsp.edges.size() == 3);
}

TEST_CASE("layout: all levels nests children inside their groups (14.3)") {
  test::FixtureCopy p("wavkit");
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  REQUIRE((*idx)->update());
  layout::SugiyamaEngine eng;
  auto boxes = layout::layout_all_levels((*idx)->snapshot(), false, fixed(), eng);
  auto find = [&](const std::string& id) {
    for (const auto& b : boxes) {
      if (b.id == id) return b;
    }
    FAIL("missing " << id);
    return layout::Box{};
  };
  auto stream = find("stream");
  auto ring = find("stream.ring");
  CHECK(stream.group);
  CHECK(ring.depth == 1);
  CHECK(ring.x >= stream.x);
  CHECK(ring.y >= stream.y);
  CHECK(ring.x + ring.w <= stream.x + stream.w);
  CHECK(ring.y + ring.h <= stream.y + stream.h);
}

TEST_CASE("layout: one group opened in place, the others stay closed") {
  test::FixtureCopy p("wavkit");
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  REQUIRE((*idx)->update());
  layout::SugiyamaEngine eng;
  auto boxes = layout::layout_nested((*idx)->snapshot(), "", [](const std::string& id) { return id == "stream"; }, false,
                                     fixed(), eng);
  std::map<std::string, layout::Box> by;
  for (const auto& b : boxes) by[b.id] = b;
  REQUIRE(by.contains("stream"));
  CHECK(by["stream"].group);  // a frame
  REQUIRE(by.contains("stream.ring"));
  CHECK_FALSE(by["dsp"].group);  // closed: one box, no children
  CHECK_FALSE(by.contains("dsp.chain"));
  const auto& s = by["stream"];
  const auto& r = by["stream.ring"];
  CHECK(r.x >= s.x + layout::kFramePad - 0.5);
  CHECK(r.y >= s.y + layout::kFrameTitle - 0.5);
  CHECK(r.x + r.w <= s.x + s.w + 0.5);
  CHECK(r.y + r.h <= s.y + s.h + 0.5);
  // Top-level boxes do not overlap.
  std::vector<layout::Box> top;
  for (const auto& b : boxes) {
    if (b.depth == 0) top.push_back(b);
  }
  for (std::size_t i = 0; i < top.size(); ++i) {
    for (std::size_t j = i + 1; j < top.size(); ++j) {
      bool apart = top[i].x + top[i].w <= top[j].x || top[j].x + top[j].w <= top[i].x || top[i].y + top[i].h <= top[j].y ||
                   top[j].y + top[j].h <= top[i].y;
      CHECK_MESSAGE(apart, top[i].id << " overlaps " << top[j].id);
    }
  }
}

// --- the improved layered layout and its routes (docs/adr/0042) ---------------------------------------

namespace {

bool inside(const layout::Point& p, const layout::Pos& at, const layout::Node& n) {
  return p.x > at.x + 1 && p.x < at.x + n.w - 1 && p.y > at.y + 1 && p.y < at.y + n.h - 1;
}

// Every route point lies outside every block, and a route's horizontal piece across a column
// passes no block either.
void check_routes_clear(const layout::Graph& g, layout::Layout& l) {
  for (const auto& r : l.routes) {
    for (std::size_t i = 0; i < r.points.size(); ++i) {
      for (const auto& n : g.nodes) {
        const auto& at = l.pos[n.id];
        CHECK_FALSE(inside(r.points[i], at, n));
        if (i + 1 < r.points.size() && r.points[i].y == r.points[i + 1].y) {
          double x0 = std::min(r.points[i].x, r.points[i + 1].x);
          double x1 = std::max(r.points[i].x, r.points[i + 1].x);
          bool through = r.points[i].y > at.y && r.points[i].y < at.y + n.h && x0 < at.x + n.w && at.x < x1;
          CHECK_MESSAGE(!through, r.from << "->" << r.to << " passes over " << n.id);
        }
      }
    }
  }
}

}  // namespace

TEST_CASE("layout: a long line is routed straight between the blocks of the ranks it passes over") {
  layout::Graph g;
  for (const char* id : {"a", "b", "c", "d", "x", "y"}) g.nodes.push_back({id, 140, 50});
  g.edges = {{"a", "b", "call"}, {"b", "c", "call"}, {"c", "d", "call"}, {"a", "d", "call"}, {"x", "b", "use"}, {"y", "c", "use"}};
  auto l = layout::SugiyamaEngine().layout(g);
  REQUIRE(l);
  REQUIRE(l->routes.size() == g.edges.size());
  const layout::Route* ad = nullptr;
  for (const auto& r : l->routes) {
    if (r.from == "a" && r.to == "d") ad = &r;
    if (r.from == "a" && r.to == "b") CHECK(r.points.empty());  // neighbor ranks: a plain curve
  }
  REQUIRE(ad != nullptr);
  REQUIRE(ad->points.size() == 4);  // across the columns of b and c
  CHECK(ad->points.front().x < ad->points.back().x);
  CHECK(ad->points.front().y == doctest::Approx(ad->points.back().y));  // straight
  check_routes_clear(g, *l);
  auto again = layout::SugiyamaEngine().layout(g);
  REQUIRE(again);
  CHECK(again->routes[3].points.size() == l->routes[3].points.size());
  CHECK(again->pos["x"].y == l->pos["x"].y);
}

TEST_CASE("layout: the order inside ranks removes crossings that a plain order has") {
  layout::Graph g;
  for (const char* id : {"p", "q", "r", "s", "t", "u"}) g.nodes.push_back({id, 100, 40});
  // In the input order p,q / r,s / t,u every pair of lines would cross; a drawing without crossings exists.
  g.edges = {{"p", "s", "call"}, {"q", "r", "call"}, {"s", "u", "call"}, {"r", "t", "call"}};
  auto l = layout::SugiyamaEngine().layout(g);
  REQUIRE(l);
  auto above = [&](const char* a, const char* b) { return l->pos[a].y < l->pos[b].y; };
  CHECK(above("p", "q") == above("s", "r"));
  CHECK(above("s", "r") == above("u", "t"));
}

TEST_CASE("layout: a hub linked both ways goes first, a pair both ways is one edge, no loops") {
  layout::Graph g;
  g.nodes.push_back({"hub", 160, 48});
  g.nodes.push_back({"glossary", 160, 48});
  for (int i = 0; i < 6; ++i) g.nodes.push_back({"n" + std::to_string(i), 160, 48});
  for (int i = 0; i < 6; ++i) {
    auto id = "n" + std::to_string(i);
    g.edges.push_back({"hub", id, "ref"});
    g.edges.push_back({id, "hub", "ref"});
    g.edges.push_back({id, "glossary", "ref"});
  }
  auto l = layout::SugiyamaEngine().layout(g);
  REQUIRE(l);
  for (int i = 0; i < 6; ++i) {
    auto id = "n" + std::to_string(i);
    CHECK(l->pos["hub"].x < l->pos[id].x);
    CHECK(l->pos[id].x < l->pos["glossary"].x);
  }
  for (const auto& r : l->routes) CHECK(r.points.empty());  // every line joins neighbor ranks
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    for (std::size_t j = i + 1; j < g.nodes.size(); ++j) {
      CHECK_FALSE(overlap(l->pos[g.nodes[i].id], g.nodes[i], l->pos[g.nodes[j].id], g.nodes[j]));
    }
  }
}

TEST_CASE("layout: a line against the flow is routed back between the blocks, single blocks go in rows") {
  layout::Graph g;
  for (const char* id : {"a", "b", "c", "d"}) g.nodes.push_back({id, 120, 44});
  for (int i = 0; i < 12; ++i) g.nodes.push_back({"lone" + std::to_string(i), 100, 40});
  g.edges = {{"a", "b", "data"}, {"b", "c", "data"}, {"c", "d", "data"}, {"d", "a", "event"}};
  auto l = layout::SugiyamaEngine().layout(g);
  REQUIRE(l);
  const layout::Route* back = nullptr;
  for (const auto& r : l->routes) {
    if (r.from == "d") back = &r;
  }
  REQUIRE(back != nullptr);
  REQUIRE(back->points.size() == 4);
  CHECK(back->points.front().x > back->points.back().x);  // from d on the right back to a on the left
  check_routes_clear(g, *l);
  double lone_top = 1e9;
  double lone_bottom = 0;
  for (int i = 0; i < 12; ++i) {
    const auto& p = l->pos["lone" + std::to_string(i)];
    lone_top = std::min(lone_top, p.y);
    lone_bottom = std::max(lone_bottom, p.y + 40);
    CHECK(p.y > l->pos["a"].y);  // under the linked part
  }
  CHECK(lone_bottom - lone_top < 6 * 40);  // in rows, not one column
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    for (std::size_t j = i + 1; j < g.nodes.size(); ++j) {
      CHECK_FALSE(overlap(l->pos[g.nodes[i].id], g.nodes[i], l->pos[g.nodes[j].id], g.nodes[j]));
    }
  }
}
