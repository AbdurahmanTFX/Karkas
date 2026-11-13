#include <doctest/doctest.h>

#include "kk/agents/edit.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {
core::Schema load(const test::TempDir& d) {
  auto s = core::load_schema(d.path());
  REQUIRE(s);
  return *s;
}
void no_errors(const test::TempDir& d, std::vector<std::string> tolerate = {}) {
  for (const auto& i : core::check_schema(load(d))) {
    if (i.severity != 'E') continue;
    if (std::find(tolerate.begin(), tolerate.end(), i.code) != tolerate.end()) continue;
    FAIL(i.str());
  }
}
agents::AppliedEdit apply(const test::TempDir& d, const kk::Result<agents::EditOp>& op) {
  REQUIRE_MESSAGE(op, (op ? "" : op.error().message));
  auto a = agents::apply_edit(d.path(), *op);
  REQUIRE_MESSAGE(a, (a ? "" : a.error().message));
  return *a;
}
}  // namespace

TEST_CASE("edit: add a leaf and a group, undo restores the files (stage 9)") {
  test::FixtureCopy p("wavkit");
  auto op = agents::op_add_block(load(p), "dsp", "reverb", core::BlockKind::Leaf, "apply Schroeder reverb to frames");
  REQUIRE(op);
  CHECK(core::contains(agents::preview_edit(p.path(), *op), "+purpose apply Schroeder reverb to frames"));
  auto a = apply(p, op);
  CHECK(std::filesystem::exists(p / "blocks/dsp/reverb/block.ksl"));
  no_errors(p);
  REQUIRE(agents::undo_edit(p.path(), a));
  CHECK_FALSE(std::filesystem::exists(p / "blocks/dsp/reverb"));
  auto g = apply(p, agents::op_add_block(load(p), "", "fx", core::BlockKind::Group, "extra effects"));
  CHECK(std::filesystem::exists(p / "blocks/fx/block.ksl"));
  no_errors(p, {"E06"});
  CHECK_FALSE(agents::op_add_block(load(p), "dsp.gain", "x", core::BlockKind::Leaf, "p"));  // parent is a leaf
  CHECK_FALSE(agents::op_add_block(load(p), "dsp", "gain", core::BlockKind::Leaf, "p"));    // exists
}

TEST_CASE("edit: ports — add, remove (links to it are cut), undo") {
  test::FixtureCopy p("wavkit");
  auto before = test::read(p / "blocks/stream/ring/block.ksl");
  auto a = apply(p, agents::op_add_port(load(p), "dsp.gain", core::PortDir::Out, "level", "float"));
  CHECK(core::contains(test::read(p / "blocks/dsp/gain/block.ksl"), "out     level float"));
  auto f = apply(p, agents::op_add_port(load(p), "dsp.gain", core::PortDir::Fn, "", "reset() -> void"));
  CHECK(core::contains(test::read(p / "blocks/dsp/gain/block.ksl"), "fn      reset() -> void"));
  auto r = apply(p, agents::op_remove_port(load(p), "stream.pump", "frames"));
  CHECK_FALSE(core::contains(test::read(p / "blocks/stream/pump/block.ksl"), "out     frames"));
  CHECK(core::contains(test::read(p / "blocks/stream/ring/block.ksl"), "in      push Frame[] sync=lock_free rate=48k/s"));
  no_errors(p);
  REQUIRE(agents::undo_edit(p.path(), r));
  CHECK(test::read(p / "blocks/stream/ring/block.ksl") == before);
  CHECK_FALSE(agents::op_add_port(load(p), "dsp", core::PortDir::In, "x", "T"));  // group
}

TEST_CASE("edit: connect out → in and fn → call on the picture") {
  test::FixtureCopy p("wavkit");
  apply(p, agents::op_connect(load(p), "stream.ring", "pop", "mix"));
  CHECK(core::contains(test::read(p / "blocks/mix/block.ksl"), "in      pop Frame[] from=stream.ring:pop"));
  apply(p, agents::op_connect(load(p), "dsp.gain", "set_gain", "mix"));
  CHECK(core::contains(test::read(p / "blocks/mix/block.ksl"), "call    dsp.gain:set_gain"));
  // Onto an existing in port: it gains from=.
  apply(p, agents::op_add_port(load(p), "io.writer", core::PortDir::In, "aux", "Frame[]"));
  apply(p, agents::op_connect(load(p), "stream.ring", "pop", "io.writer", "aux"));
  CHECK(core::contains(test::read(p / "blocks/io/writer/block.ksl"), "in      aux Frame[] from=stream.ring:pop"));
  no_errors(p);
  CHECK_FALSE(agents::op_connect(load(p), "stream", "x", "dsp"));  // groups
  CHECK_FALSE(agents::op_connect(load(p), "dsp.gain", "set_gain", "mix"));  // duplicate call
}

TEST_CASE("edit: rename moves the folder and updates every reference, undo moves it back") {
  test::FixtureCopy p("wavkit");
  auto chain = test::read(p / "blocks/dsp/chain/block.ksl");
  auto op = agents::op_rename(load(p), "stream.ring", "buffer");
  REQUIRE(op);
  CHECK(core::contains(agents::preview_edit(p.path(), *op), "move blocks/stream/ring -> blocks/stream/buffer"));
  auto a = apply(p, op);
  CHECK(std::filesystem::exists(p / "blocks/stream/buffer/impl/ring.cpp"));
  CHECK(core::contains(test::read(p / "blocks/stream/buffer/block.ksl"), "@block stream.buffer"));
  CHECK(core::contains(test::read(p / "blocks/dsp/chain/block.ksl"), "from=stream.buffer:pop"));
  no_errors(p);
  REQUIRE(agents::undo_edit(p.path(), a));
  CHECK(std::filesystem::exists(p / "blocks/stream/ring/block.ksl"));
  CHECK(test::read(p / "blocks/dsp/chain/block.ksl") == chain);
  // A group rename carries its children.
  apply(p, agents::op_rename(load(p), "dsp", "fx"));
  CHECK(core::contains(test::read(p / "blocks/fx/chain/block.ksl"), "call    fx.gain:process"));
  CHECK(core::contains(test::read(p / "blocks/mix/block.ksl"), "from=fx.chain:out"));
  no_errors(p);
}

TEST_CASE("edit: merge and split") {
  test::FixtureCopy p("wavkit");
  apply(p, agents::op_add_block(load(p), "dsp", "tone", core::BlockKind::Leaf, "generate a test tone"));
  apply(p, agents::op_add_port(load(p), "dsp.tone", core::PortDir::Fn, "", "tone(float) -> void"));
  apply(p, agents::op_connect(load(p), "dsp.tone", "tone", "dsp.chain"));
  auto m = apply(p, agents::op_merge(load(p), "dsp.gain", "dsp.tone"));
  CHECK_FALSE(std::filesystem::exists(p / "blocks/dsp/tone"));
  CHECK(core::contains(test::read(p / "blocks/dsp/gain/block.ksl"), "fn      tone(float) -> void"));
  CHECK(core::contains(test::read(p / "blocks/dsp/chain/block.ksl"), "call    dsp.gain:tone"));
  no_errors(p);
  CHECK_FALSE(agents::op_merge(load(p), "dsp.gain", "dsp.echo"));  // echo has code
  auto s = apply(p, agents::op_split(load(p), "dsp.echo", "echo_cfg", {"set_params"}));
  CHECK(core::contains(test::read(p / "blocks/dsp/echo_cfg/block.ksl"), "fn      set_params(size_t, float, float) -> void"));
  CHECK_FALSE(core::contains(test::read(p / "blocks/dsp/echo/block.ksl"), "set_params"));
  no_errors(p);
  REQUIRE(agents::undo_edit(p.path(), s));
  CHECK(core::contains(test::read(p / "blocks/dsp/echo/block.ksl"), "set_params"));
  CHECK_FALSE(std::filesystem::exists(p / "blocks/dsp/echo_cfg"));
}

TEST_CASE("edit: remove links of every kind") {
  test::FixtureCopy p("wavkit");
  apply(p, agents::op_disconnect(load(p), "data", "stream.ring", "pop", "dsp.chain", "in"));
  CHECK(core::contains(test::read(p / "blocks/dsp/chain/block.ksl"), "in      in Frame[] sync=lock_free\n"));
  apply(p, agents::op_disconnect(load(p), "call", "dsp.chain", "", "dsp.echo", "process"));
  CHECK_FALSE(core::contains(test::read(p / "blocks/dsp/chain/block.ksl"), "dsp.echo"));
  apply(p, agents::op_disconnect(load(p), "use", "mix", "", "shared.types", ""));
  CHECK_FALSE(core::contains(test::read(p / "blocks/mix/block.ksl"), "use"));
  no_errors(p);
  CHECK_FALSE(agents::op_disconnect(load(p), "call", "mix", "", "dsp.gain", "process"));
}

TEST_CASE("edit: move a block under another group and set its purpose") {
  test::FixtureCopy p("wavkit");
  apply(p, agents::op_add_block(load(p), "root", "audio", core::BlockKind::Group, "audio processing", ""));
  auto op = agents::op_move(load(p), "dsp", "audio");
  REQUIRE(op);
  CHECK(core::contains(agents::preview_edit(p.path(), *op), "move blocks/dsp -> blocks/audio/dsp"));
  auto a = apply(p, op);
  CHECK(core::contains(test::read(p / "blocks/audio/dsp/chain/block.ksl"), "@block audio.dsp.chain"));
  CHECK(core::contains(test::read(p / "blocks/mix/block.ksl"), "from=audio.dsp.chain:out"));
  no_errors(p);
  // Back to the top level, which leaves audio empty (allowed, then removed).
  apply(p, agents::op_move(load(p), "audio.dsp", "root"));
  CHECK(core::contains(test::read(p / "blocks/mix/block.ksl"), "from=dsp.chain:out"));
  apply(p, agents::op_remove_block(load(p), "audio"));
  no_errors(p);
  CHECK_FALSE(agents::op_move(load(p), "dsp", "dsp.chain"));  // not a group / into itself
  CHECK_FALSE(agents::op_move(load(p), "dsp", "root"));       // already there
  CHECK_FALSE(agents::op_move(load(p), "stream.ring", "dsp.gain"));
  apply(p, agents::op_set_purpose(load(p), "mix", "sums the tracks into one stream"));
  CHECK(core::contains(test::read(p / "blocks/mix/block.ksl"), "purpose sums the tracks into one stream"));
  no_errors(p);
}

TEST_CASE("edit: a script of edits in one go, stops at the first failing line") {
  test::FixtureCopy p("wavkit");
  auto r = agents::apply_edit_script(p.path(), "# regroup\n"
                                               "add-group audio \"audio processing: \\\"dsp\\\" and mixing\"\n"
                                               "move dsp audio\n"
                                               "move mix audio\n"
                                               "purpose audio.mix \"sums the tracks into one stream\"\n");
  CHECK(r.error.empty());
  CHECK(r.applied == 4);
  CHECK(core::contains(test::read(p / "blocks/audio/block.ksl"), "audio processing: \"dsp\" and mixing"));
  CHECK(core::contains(test::read(p / "blocks/audio/mix/block.ksl"), "purpose sums the tracks into one stream"));
  no_errors(p);
  auto bad = agents::apply_edit_script(p.path(), "move audio.dsp root\nmove nowhere audio\nmove stream audio\n");
  CHECK(bad.applied == 1);
  CHECK(bad.error.starts_with("line 2: "));
  CHECK(std::filesystem::exists(p / "blocks/stream/block.ksl"));  // line 3 was not run
  CHECK(agents::apply_edit_script(p.path(), "teleport x y\n").error.starts_with("line 1: unknown command"));
  CHECK(agents::apply_edit_script(p.path(), "move x\n").error.starts_with("line 1: wrong number of arguments"));
}
