#include <doctest/doctest.h>

#include "kk/code/drift.hpp"
#include "kk/code/gen.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"
#include "kk/runner/runner.hpp"
#include "kk/test/support.hpp"

using namespace kk;
using namespace std::chrono_literals;

namespace {
core::Schema schema_of(const std::filesystem::path& p) {
  auto s = core::load_schema(p);
  REQUIRE(s);
  return *s;
}
}  // namespace

TEST_CASE("gen cmake: deterministic, one target per block, links from the schema (5.5)") {
  test::FixtureCopy p("wavkit");
  auto s = schema_of(p.path());
  auto a = code::gen_cmake(s);
  CHECK(a == code::gen_cmake(s));
  CHECK(a == test::read(p / "CMakeLists.txt"));  // the fixture holds the generated file
  auto h = code::cmake_hashes(a);
  CHECK(h.generated);
  CHECK(h.declared == h.actual);
  CHECK(core::contains(a, "add_library(stream_ring STATIC ${_src})"));
  CHECK(core::contains(a, "target_link_libraries(stream_ring PRIVATE shared_probe shared_types stream_pump)"));
  CHECK(core::contains(a, "add_library(shared_types INTERFACE)"));
  CHECK(core::contains(a, "add_library(dsp INTERFACE)"));
  CHECK(core::contains(a, "target_link_libraries(dsp INTERFACE dsp_chain dsp_echo dsp_gain dsp_lowpass)"));
  CHECK(core::contains(a, "add_test(NAME stream.ring/contract COMMAND stream_ring_tests \"-sf=*/tests/contract/*\")"));
  CHECK(core::contains(a, "set_tests_properties(stream.ring/contract PROPERTIES LABELS \"stream.ring;stream\")"));
  CHECK(core::contains(a, "add_executable(app ${_app})"));
  CHECK(core::contains(a, "OUTPUT_NAME wavkit"));
  CHECK(core::contains(a, "add_test(NAME dsp/integration COMMAND dsp_integration)"));
  CHECK(core::contains(a, "include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/user.cmake\" OPTIONAL)"));
  auto w = code::write_cmake(s);
  REQUIRE(w);
  CHECK(w->written.empty());  // unchanged
}

TEST_CASE("gen cmake: a tests/ folder holding only .gitkeep gives no test program and no empty test") {
  test::FixtureCopy p("wavkit");
  // mix has tests/unit/.gitkeep and one contract test: a contract test, no unit test.
  auto a = code::gen_cmake(schema_of(p.path()));
  CHECK(core::contains(a, "add_test(NAME mix/contract COMMAND mix_tests"));
  CHECK_FALSE(core::contains(a, "add_test(NAME mix/unit"));
  std::filesystem::remove(p / "blocks/mix/tests/contract/mix_contract.cpp");
  auto b = code::gen_cmake(schema_of(p.path()));
  CHECK_FALSE(core::contains(b, "mix_tests"));
  // Every glob covers the extensions has_cpp counts.
  CHECK(core::contains(b, "/blocks/stream/ring/tests/*.cc\""));
  CHECK(core::contains(b, "/blocks/stream/ring/impl/*.cxx\""));
}

TEST_CASE("gen mocks: classes inheriting the neighbor interfaces with call records") {
  test::FixtureCopy p("wavkit");
  std::filesystem::remove(p / "blocks/stream/pump/mocks/io_reader_mock.hpp");
  auto r = code::write_mocks(schema_of(p.path()), "stream.pump");
  REQUIRE(r);
  REQUIRE(r->written.size() == 1);
  auto m = test::read(p / "blocks/stream/pump/mocks/io_reader_mock.hpp");
  CHECK(core::contains(m, "karkas:generated"));
  CHECK(core::contains(m, "#include \"reader.hpp\""));
  CHECK(core::contains(m, "class MockIReader : public ::wk::IReader {"));
  CHECK(core::contains(m, "std::function<Result<AudioFormat>(const std::string&)> on_open;"));
  CHECK(core::contains(m, "std::size_t frames(std::span<Frame> out) override {"));
  CHECK(m == test::read(test::fixture("wavkit") / "blocks/stream/pump/mocks/io_reader_mock.hpp"));
  // A hand-written mock (no marker) is kept.
  test::write(p / "blocks/stream/pump/mocks/io_reader_mock.hpp", "// mine\n");
  auto again = code::write_mocks(schema_of(p.path()), "stream.pump");
  REQUIRE(again);
  CHECK(again->skipped.size() == 1);
}

TEST_CASE("gen skel and probe for a new leaf") {
  test::FixtureCopy p("wavkit");
  test::write(p / "blocks/dsp/reverb/block.ksl",
              "@block dsp.reverb\npurpose apply Schroeder reverb to frames\nkind    leaf\nthread  main\n"
              "fn      process(Frame[]) -> void\nuse     shared.types\ninv     zero mix leaves samples unchanged\n");
  test::write(p / "blocks/dsp/reverb/api/reverb.hpp", "#pragma once\nstruct IReverb { virtual ~IReverb() = default; };\n");
  auto r = code::write_skel(schema_of(p.path()), "dsp.reverb");
  REQUIRE(r);
  CHECK(r->written.size() == 4);
  auto impl = test::read(p / "blocks/dsp/reverb/impl/reverb.cpp");
  CHECK(core::contains(impl, "karkas:skeleton"));
  CHECK(core::contains(impl, "#include \"reverb.hpp\""));
  CHECK(std::filesystem::exists(p / "blocks/dsp/reverb/tests/contract/.gitkeep"));
  auto pr = code::write_probe(schema_of(p.path()));
  REQUIRE(pr);
  CHECK(pr->written.empty());  // probe header already up to date in the fixture
}

TEST_CASE("build: the reference project builds with the generated CMake, tests pass, probes are recorded") {
  test::FixtureCopy p("wavkit");
  auto build_dir = p / "build";
  runner::BuildOptions bo{p.path(), build_dir, KK_DOCTEST_INCLUDE_DIR, "", 600s, {}};
  auto b = runner::build(bo);
  REQUIRE(b);
  REQUIRE_MESSAGE(b->ok, b->summary << "\n" << b->log.substr(b->log.size() > 4000 ? b->log.size() - 4000 : 0));
  auto t = runner::test({p.path(), build_dir, {}, 120s, p / ".karkas/probes", "build-test"});
  REQUIRE(t);
  CHECK_MESSAGE(t->ok, t->text);
  CHECK(core::contains(t->text, "tests stream.ring contract 3/3 unit 1/1"));
  CHECK(core::contains(t->text, "tests dsp integration 1/1"));
  // Only one subtree, by label (ctest -L stream).
  auto sub = runner::test({p.path(), build_dir, {"stream"}, 120s, {}, {}});
  REQUIRE(sub);
  CHECK(sub->blocks.size() == 2);

  // Run the program end to end with probes on.
  std::string wav = (p / "in.wav").string();
  {
    std::string hdr = "RIFF";
    auto put = [&](std::uint32_t v, int n) {
      for (int i = 0; i < n; ++i) hdr.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    const std::uint32_t frames = 48000;
    put(36 + frames * 4, 4);
    hdr += "WAVEfmt ";
    put(16, 4);
    put(1, 2);
    put(2, 2);
    put(48000, 4);
    put(48000 * 4, 4);
    put(4, 2);
    put(16, 2);
    hdr += "data";
    put(frames * 4, 4);
    for (std::uint32_t i = 0; i < frames * 2; ++i) put(static_cast<std::uint16_t>((i % 200) * 100), 2);
    test::write(wav, hdr);
  }
  auto run = runner::run({{(build_dir / "wavkit").string(), "--gain", "0.5", "--echo", "20", "-o",
                           (p / "out.wav").string(), wav},
                          p.path(),
                          60s,
                          {{"KARKAS_PROBE_DIR", (p / ".karkas/probes").string()}, {"KARKAS_PROBE_RUN", "app"}}});
  REQUIRE(run);
  CHECK_MESSAGE(run->ok(), run->output);
  CHECK(std::filesystem::file_size(p / "out.wav") == 44 + 48000 * 4);
  CHECK(std::filesystem::exists(p / ".karkas/probes/app.jsonl"));
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  REQUIRE((*idx)->update());
  CHECK(core::contains((*idx)->probes("stream"), "probe stream.pump->stream.ring fill"));

  // The composition root has unit tests too: its impl/ without main.cpp, linked with the leaves.
  test::write(p / "blocks/app/tests/unit/app_test.cpp",
              "#include <doctest/doctest.h>\n\nint run_app(int argc, char** argv);\n\n"
              "TEST_CASE(\"app: the composition root links without main.cpp\") { CHECK(&run_app != nullptr); }\n");
  // A standalone test with a main() of its own is a program of its own, not a second main() in
  // the doctest program (flappy-karkas: "multiple definition of main" stopped the whole build).
  test::write(p / "blocks/app/tests/unit/test_gate.cpp",
              "#include <cstdio>\nint run_app(int argc, char** argv);\n"
              "int main() { std::puts(\"gate ok\"); return &run_app != nullptr ? 0 : 1; }\n");
  REQUIRE(code::write_cmake(schema_of(p.path())));
  // Test files removed after the CMakeLists.txt was written: the configure goes on without that
  // test program instead of failing for the whole project ("No SOURCES given to target").
  std::filesystem::remove(p / "blocks/mix/tests/contract/mix_contract.cpp");
  auto again = runner::build(bo);
  REQUIRE(again);
  REQUIRE_MESSAGE(again->ok, again->summary);
  auto app_tests = runner::test({p.path(), build_dir, {"app"}, 120s, {}, {}});
  REQUIRE(app_tests);
  CHECK_MESSAGE(core::contains(app_tests->text, "tests app unit 2/2"), app_tests->text);
  // A tests/ folder with only .gitkeep is no test program at all.
  auto cm = code::gen_cmake(schema_of(p.path()));
  CHECK_FALSE(core::contains(cm, "add_executable(mix_tests"));
  CHECK(core::contains(cm, "add_executable(app_tests ${_app_tests} ${_app_lib})"));
  CHECK(core::contains(cm, "list(REMOVE_ITEM _app_tests \"${CMAKE_CURRENT_SOURCE_DIR}/blocks/app/tests/unit/test_gate.cpp\")"));
  CHECK(core::contains(cm, "add_test(NAME app/unit:test_gate COMMAND app_test_gate)"));

  // A leaf without a link physically cannot include another leaf's api (5.5).
  auto mix = p / "blocks/mix/impl/mix.cpp";
  test::write(mix, "#include \"ring.hpp\"\n" + test::read(mix));
  auto bad = runner::build(bo);
  REQUIRE(bad);
  CHECK_FALSE(bad->ok);
  CHECK(core::contains(bad->summary, "ring.hpp"));
  CHECK(core::contains(bad->summary, "blocks/mix/impl/mix.cpp"));
}
