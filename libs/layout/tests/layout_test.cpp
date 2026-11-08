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
