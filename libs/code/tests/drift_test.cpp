#include <doctest/doctest.h>

#include <algorithm>

#include "kk/code/analyzer.hpp"
#include "kk/code/drift.hpp"
#include "kk/code/gen.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

// A fresh copy of the reference project; every D check gets its own deliberately
// broken variant (stage 5 acceptance).
struct Broken : test::FixtureCopy {
  Broken() : FixtureCopy("wavkit") {}
  std::vector<core::Issue> drift() {
    auto s = core::load_schema(path());
    REQUIRE(s);
    return code::check_drift(*s);
  }
  std::vector<core::Issue> with(std::string_view code) {
    std::vector<core::Issue> out;
    for (auto& i : drift()) {
      if (i.code == code) out.push_back(i);
    }
    return out;
  }
  void edit(const char* rel, std::string_view from, std::string_view to) {
    auto p = path() / rel;
    auto t = test::read(p);
    REQUIRE_MESSAGE(core::contains(t, from), rel << " does not contain " << from);
    test::write(p, core::replace_all(t, from, to));
  }
};

}  // namespace

TEST_CASE("drift: the reference project is clean") {
  Broken p;
  auto issues = p.drift();
  for (const auto& i : issues) MESSAGE(i.str());
  CHECK(issues.empty());
}

TEST_CASE("drift D01: include of another block's impl/") {
  Broken p;
  test::write(p / "blocks/stream/ring/impl/ring_internal.hpp", "#pragma once\n");
  p.edit("blocks/stream/pump/impl/pump.cpp", "#include \"reader.hpp\"", "#include \"reader.hpp\"\n#include \"ring_internal.hpp\"");
  auto d = p.with("D01");
  REQUIRE(d.size() == 1);
  CHECK(d[0].severity == 'E');
  CHECK(d[0].block == "stream.pump");
  CHECK(d[0].loc.file == "blocks/stream/pump/impl/pump.cpp");
}

TEST_CASE("drift D02: include of an api/ without a link") {
  Broken p;
  p.edit("blocks/stream/pump/impl/pump.cpp", "#include \"reader.hpp\"", "#include \"reader.hpp\"\n#include \"chain.hpp\"");
  auto d = p.with("D02");
  REQUIRE(d.size() == 1);
  CHECK(d[0].str() == "blocks/stream/pump/impl/pump.cpp:10: E D02 stream.pump: includes dsp.chain api without link");
  // The entry may include every leaf api (8.2): no D02 for compose.cpp.
  CHECK(std::none_of(d.begin(), d.end(), [](const core::Issue& i) { return i.block == "app"; }));
}

TEST_CASE("drift D03: port type missing from the api/") {
  Broken p;
  p.edit("blocks/stream/ring/block.ksl", "out     pop Frame[]", "out     pop Chunk[]");
  auto d = p.with("D03");
  REQUIRE(d.size() == 1);
  CHECK(core::contains(d[0].message, "type 'Chunk' of out pop not found"));
}

TEST_CASE("drift D04: fn of the card not declared in api/ (and soft parameter check)") {
  Broken p;
  p.edit("blocks/stream/ring/block.ksl", "fn      fill() -> size_t", "fn      fill() -> size_t\nfn      reset() -> void");
  p.edit("blocks/dsp/gain/block.ksl", "fn      set_gain(float) -> void", "fn      set_gain(float, float) -> void");
  auto d = p.with("D04");
  REQUIRE(d.size() == 2);
  CHECK(d[0].block == "dsp.gain");
  CHECK(d[0].severity == 'W');
  CHECK(d[1].block == "stream.ring");
  CHECK(d[1].severity == 'E');
  CHECK(core::contains(d[1].message, "reset()"));
}

TEST_CASE("drift D05: api/ changed after freezing") {
  Broken p;
  auto s = core::load_schema(p.path());
  REQUIRE(s);
  auto files = index::collect_code_files(*s);
  REQUIRE(index::StateStore(p.path()).freeze_api("stream.ring", index::block_api_hash(files, "stream.ring")));
  CHECK(p.with("D05").empty());
  p.edit("blocks/stream/ring/api/ring.hpp", "[[nodiscard]] virtual std::size_t fill() const = 0;",
         "[[nodiscard]] virtual std::size_t fill() const = 0;\n  virtual void reset() = 0;");
  auto d = p.with("D05");
  REQUIRE(d.size() == 1);
  CHECK(d[0].block == "stream.ring");
}

TEST_CASE("drift D06: orphan code files") {
  Broken p;
  test::write(p / "blocks/stream/ring/scratch.cpp", "int x;\n");
  test::write(p / "src/orphan.cpp", "int y;\n");
  auto d = p.with("D06");
  REQUIRE(d.size() == 2);
  CHECK(d[0].loc.file == "blocks/stream/ring/scratch.cpp");
  CHECK(d[1].loc.file == "src/orphan.cpp");
}

TEST_CASE("drift D07: generated CMakeLists.txt edited by hand or stale") {
  Broken p;
  p.edit("CMakeLists.txt", "enable_testing()", "enable_testing()\nadd_compile_options(-O3)");
  auto d = p.with("D07");
  REQUIRE(d.size() == 1);
  CHECK(d[0].severity == 'E');
  auto s = core::load_schema(p.path());
  REQUIRE(code::write_cmake(*s));
  CHECK(p.with("D07").empty());
  p.edit("blocks/dsp/gain/block.ksl", "use     shared.types", "use     shared.types\nuse     shared.log");
  auto stale = p.with("D07");
  REQUIRE(stale.size() == 1);
  CHECK(stale[0].severity == 'W');
}

TEST_CASE("drift D08: leaf without contract tests") {
  Broken p;
  std::filesystem::remove_all(p / "blocks/dsp/gain/tests/contract");
  auto d = p.with("D08");
  REQUIRE(d.size() == 1);
  CHECK(d[0].block == "dsp.gain");
  CHECK(d[0].severity == 'W');
}

TEST_CASE("drift D09: call into a block without a call link (heuristic)") {
  Broken p;
  p.edit("blocks/mix/impl/mix.cpp", "    out.assign(len, Frame{});", "    out.assign(len, Frame{});\n    set_cutoff(1.0f, 2.0f);");
  auto d = p.with("D09");
  REQUIRE(d.size() == 1);
  CHECK(d[0].severity == 'W');
  CHECK(d[0].block == "mix");
  CHECK(core::contains(d[0].message, "set_cutoff() of dsp.lowpass"));
}

TEST_CASE("drift: the index stores D issues and marks blocks") {
  Broken p;
  p.edit("blocks/stream/pump/impl/pump.cpp", "#include \"reader.hpp\"", "#include \"reader.hpp\"\n#include \"chain.hpp\"");
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  auto an = std::make_unique<code::CppAnalyzer>();
  REQUIRE((*idx)->update({false, an.get()}));
  auto snap = (*idx)->snapshot();
  CHECK(snap.block("stream.pump")->status == "drift");
  CHECK(core::contains((*idx)->drift("stream"), "drift E D02 stream.pump blocks/stream/pump/impl/pump.cpp:10 includes dsp.chain api without link"));
  // code_refs were resolved.
  auto q = (*idx)->db().q("SELECT COUNT(*) FROM code_refs WHERE from_block='stream.pump' AND to_block='io.reader'");
  REQUIRE(q.step());
  CHECK(q.i64(0) >= 1);
}
