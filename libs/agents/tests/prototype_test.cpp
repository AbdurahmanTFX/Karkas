// The prototype (docs/adr/0035): the sketch file, its own problems, and the check — without a
// model — that a schema keeps its blocks and dependencies.
#include <doctest/doctest.h>

#include "kk/agents/engine.hpp"
#include "kk/agents/prototype.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;

namespace {

agents::Sketch player() {
  agents::Sketch s;
  s.idea = "a WAV player with effects";
  s.blocks = {{"io", "read and write WAV files", "group", 0, 0},
              {"stream", "", "", 200, 0},
              {"dsp", "effects", "group", 400, 0},
              {"app", "", "entry", 0, 200}};
  s.links = {{"app", "io", ""}, {"io", "stream", "frames"}, {"stream", "dsp", ""}};
  return s;
}

}  // namespace

TEST_CASE("prototype: the sketch file round-trips and an absent one is empty") {
  test::TempDir d{"proto"};
  auto none = agents::load_sketch(d.path());
  REQUIRE(none);
  CHECK(none->empty());
  auto s = player();
  s.map["stream"] = "transport";
  s.sent = "design";
  s.sent_at = 42;
  REQUIRE(agents::save_sketch(d.path(), s));
  auto back = agents::load_sketch(d.path());
  REQUIRE(back);
  CHECK(back->idea == s.idea);
  REQUIRE(back->blocks.size() == 4);
  CHECK(back->blocks[1].x == doctest::Approx(200));
  CHECK(back->blocks[0].kind == "group");
  REQUIRE(back->links.size() == 3);
  CHECK(back->links[1].note == "frames");
  CHECK(back->map.at("stream") == "transport");
  CHECK(back->sent == "design");
  REQUIRE(agents::clear_sketch(d.path()));
  CHECK(agents::load_sketch(d.path())->empty());
}

TEST_CASE("prototype: problems of the sketch itself") {
  auto s = player();
  CHECK(agents::sketch_problems(s, {}).empty());
  auto bad = s;
  bad.blocks[1].name = "Stream";
  CHECK(core::contains(agents::sketch_problems(bad, {}), "not a block name"));
  bad = s;
  bad.blocks.push_back({"io", "", "", 0, 0});
  CHECK(core::contains(agents::sketch_problems(bad, {}), "twice"));
  bad = s;
  bad.links.push_back({"app", "nowhere", ""});
  CHECK(core::contains(agents::sketch_problems(bad, {}), "neither drawn nor a block"));
  CHECK(agents::sketch_problems(bad, {"nowhere"}).empty());  // a link to a block that exists
  bad = s;
  bad.links.push_back({"io", "stream", ""});
  CHECK(core::contains(agents::sketch_problems(bad, {}), "drawn twice"));
  CHECK(core::contains(agents::sketch_problems(s, {"io"}), "exists already"));
  CHECK(core::contains(agents::sketch_problems(agents::Sketch{}, {}), "no blocks"));
  // Inside a group the ids carry the group.
  agents::Sketch g;
  g.scope = "dsp";
  g.blocks = {{"echo", "", "", 0, 0}, {"gain", "", "", 0, 0}};
  g.links = {{"dsp.echo", "dsp.gain", ""}};
  CHECK(agents::sketch_problems(g, {"dsp"}).empty());
  CHECK(core::contains(agents::sketch_text(g), "block dsp.echo"));
  CHECK(core::contains(agents::sketch_text(g), "dep   dsp.echo -> dsp.gain"));
}

TEST_CASE("prototype: a schema keeps the sketch, a rename through the map, links through subtrees") {
  auto s = player();
  agents::SchemaShape shape;
  shape.blocks = {"io", "io.reader", "transport", "transport.ring", "dsp", "app", "shared", "shared.types"};
  shape.childless_groups = {"dsp"};
  shape.links = {{"app", "io.reader"}, {"io.reader", "transport.ring"}};
  // stream was renamed to transport: without the map it is missing.
  auto c = agents::check_sketch(s, shape);
  CHECK(c.found.at("stream").empty());
  CHECK(c.kept_blocks() == 3);
  REQUIRE(c.gaps.size() == 1);
  CHECK(c.gaps[0] == "block stream is not in the schema");
  s.map["stream"] = "transport";
  c = agents::check_sketch(s, shape);
  CHECK(c.gaps.empty());
  CHECK(c.found.at("stream") == "transport");
  REQUIRE(c.links.size() == 3);
  CHECK(c.links[0] == 1);   // app -> io by app -> io.reader
  CHECK(c.links[1] == 1);   // io -> stream by io.reader -> transport.ring
  CHECK(c.links[2] == -1);  // stream -> dsp: dsp is not expanded yet
  // dsp expanded without a link from the stream: the dependency is a gap now.
  shape.childless_groups.clear();
  shape.blocks.insert("dsp.echo");
  c = agents::check_sketch(s, shape);
  REQUIRE(c.gaps.size() == 1);
  CHECK(c.gaps[0] == "dependency stream -> dsp is not kept");
  // A link in the other direction keeps it too.
  shape.links.emplace_back("dsp.echo", "transport.ring");
  CHECK(agents::check_sketch(s, shape).gaps.empty());
}

TEST_CASE("prototype: a gap is explained when the deviations name it, and the map is parsed from the architect lines") {
  auto s = player();
  agents::SchemaShape shape;
  shape.blocks = {"io", "dsp", "app"};  // no links at all
  auto c = agents::check_sketch(s, shape);
  REQUIRE(c.gaps.size() == 2);  // stream is missing (its links are not gaps of their own), app -> io is not kept
  auto open = agents::unexplained_gaps(s, c, "");
  CHECK(open.size() == c.gaps.size());
  // "streaming" does not name "stream".
  CHECK(agents::unexplained_gaps(s, c, "streaming is done by io").size() == open.size());
  auto left = agents::unexplained_gaps(s, c, "stream: merged into io, the reader thread pushes frames itself.");
  REQUIRE(left.size() == 1);
  CHECK(left[0] == "dependency app -> io is not kept");
  CHECK(agents::unexplained_gaps(s, c, "stream merged into io. app reads io through the CLI only.").empty());
  auto m = agents::parse_sketch_map("stream -> transport\n dsp => effects; app: main , bad line, x.y = x.z");
  CHECK(m.size() == 4);
  CHECK(m.at("stream") == "transport");
  CHECK(m.at("dsp") == "effects");
  CHECK(m.at("app") == "main");
  CHECK(m.at("x.y") == "x.z");
}

TEST_CASE("prototype: the task for a level that has blocks names the blocks and the dependencies") {
  agents::Sketch g;
  g.scope = "dsp";
  g.idea = "a reverb after the echo";
  g.blocks = {{"reverb", "Schroeder reverb", "leaf", 0, 0}};
  g.links = {{"dsp.echo", "dsp.reverb", "frames"}};
  auto t = agents::sketch_task(g);
  CHECK(core::contains(t, "a reverb after the echo"));
  CHECK(core::contains(t, "dsp.reverb (Schroeder reverb)"));
  CHECK(core::contains(t, "dsp.echo -> dsp.reverb (frames)"));
}

TEST_CASE("prototype: the architect keeps the prototype or explains each gap, and offers a variant as a change") {
  test::TempDir d{"proto_design"};
  test::write(d / "project.ksl", "@project calc\nlang    cpp\nstd     c++20\ntest    doctest\n");
  test::ScriptedModel model;
  test::FakeTests tests;
  agents::EngineOptions o;
  o.providers = model.factory();
  o.tests = &tests;
  auto e = agents::Engine::create(d.path(), test::test_config(), o);
  REQUIRE(e);
  agents::Sketch s;
  s.idea = "calculator on the command line";
  s.blocks = {{"app", "", "entry", 0, 0}, {"parser", "reads an expression", "", 200, 0}, {"calc", "evaluates it", "leaf", 400, 0}};
  s.links = {{"app", "parser", ""}, {"app", "calc", ""}};
  REQUIRE(agents::save_sketch(d.path(), s));
  model.tool("write_card", {{"block", "calc"},
                            {"ksl", "@block calc\npurpose parse and evaluate an expression\nkind    leaf\nthread  main\nfn      eval(string) -> double\n"}});
  model.tool("write_card", {{"block", "app"}, {"ksl", "@block app\npurpose command line\nkind    entry\nthread  main\ncall    calc:eval\n"}});
  model.tool("propose_alternative", {{"title", "a history of results"},
                                     {"reason", "the idea mentions repeating the last result; a leaf keeps it"},
                                     {"diff", "+ @block history\n+ purpose remembers the last results\n+ kind    leaf\n+ thread  main\n"}});
  model.tool("report", {{"status", "done"}, {"summary", "two blocks"}});  // refused: parser is missing, unexplained
  model.tool("report", {{"status", "done"}, {"summary", "two blocks"},
                        {"deviations", "parser: parsing is half of evaluating here, calc does both (one job: a line to a number)"}});
  auto r = (*e)->design("", &s);
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(model.calls.size() == 5);
  auto first = test::ScriptedModel::request_text(model.calls[0].req);
  CHECK(core::contains(first, "calculator on the command line"));
  CHECK(core::contains(first, "PROTOTYPE"));
  CHECK(core::contains(first, "block parser \"reads an expression\""));
  CHECK(core::contains(first, "propose_alternative"));
  auto after_refusal = test::ScriptedModel::request_text(model.calls[4].req);
  CHECK(core::contains(after_refusal, "the human's prototype is not kept: block parser is not in the schema"));
  CHECK(core::contains(*r, "prototype: 2/3 blocks kept, 1/2 dependencies kept"));
  CHECK(core::contains(*r, "deviations: parser: parsing is half"));
  CHECK(core::contains(*r, "alternative C-0001 a history of results"));
  CHECK(core::contains(test::read(d / "project.ksl"), "idea    calculator on the command line"));
  CHECK(core::contains(test::read(d / ".karkas/changes/C-0001.ksl"), "from    architect prototype"));
  CHECK_FALSE(std::filesystem::exists(d / "blocks/history"));  // only proposed
  auto saved = agents::load_sketch(d.path());
  REQUIRE(saved);
  CHECK(saved->sent == "design");
}

TEST_CASE("prototype: a group drawn at the top level gets the prototype's dependencies when it is expanded") {
  test::TempDir d{"proto_expand"};
  test::write(d / "project.ksl", "@project calc\nlang    cpp\nstd     c++20\ntest    doctest\n");
  test::write(d / "blocks/app/block.ksl", "@block app\npurpose command line\nkind    entry\nthread  main\n");
  test::write(d / "blocks/core/block.ksl", "@block core\npurpose arithmetic\nkind    group\n");
  agents::Sketch s;
  s.blocks = {{"app", "", "entry", 0, 0}, {"core", "", "group", 200, 0}};
  s.links = {{"app", "core", "expressions"}};
  s.sent = "design";
  REQUIRE(agents::save_sketch(d.path(), s));
  test::ScriptedModel model;
  test::FakeTests tests;
  agents::EngineOptions o;
  o.providers = model.factory();
  o.tests = &tests;
  auto e = agents::Engine::create(d.path(), test::test_config(), o);
  REQUIRE(e);
  model.tool("report", {{"status", "done"}});
  REQUIRE((*e)->expand("core", false));
  auto task = test::ScriptedModel::request_text(model.calls[0].req);
  CHECK(core::contains(task, "The human's prototype also asks for these dependencies of core"));
  CHECK(core::contains(task, "app -> core (expressions)"));
}
