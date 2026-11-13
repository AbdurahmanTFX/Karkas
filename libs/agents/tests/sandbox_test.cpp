#include <doctest/doctest.h>
#include <fmt/format.h>

#include <algorithm>

#include "kk/agents/sandbox.hpp"
#include "kk/agents/tools.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;

namespace {
core::Schema wav(const test::TempDir& d) {
  auto s = core::load_schema(d.path());
  REQUIRE(s);
  return *s;
}
}  // namespace

TEST_CASE("sandbox: worker reads its block and linked api/, writes impl/ and tests/unit/") {
  test::FixtureCopy p("wavkit");
  agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
  CHECK(sb.resolve_read("impl/ring.cpp"));
  CHECK(sb.resolve_read("block.ksl"));
  CHECK(sb.resolve_read("stream.pump:api/pump.hpp"));
  CHECK(sb.resolve_read("shared.types:api/types.hpp"));
  CHECK(sb.resolve_write("impl/ring.cpp"));
  CHECK(sb.resolve_write("tests/unit/new_test.cpp"));
  CHECK_FALSE(sb.resolve_write("tests/contract/x.cpp"));
  CHECK(sb.display(*sb.resolve_read("stream.pump:api/pump.hpp")) == "stream.pump:api/pump.hpp");
}

TEST_CASE("sandbox: '..' is refused") {
  test::FixtureCopy p("wavkit");
  agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
  auto r = sb.resolve_read("../pump/impl/pump.cpp");
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "'..' is not allowed"));
  CHECK_FALSE(sb.resolve_write("impl/../../pump/impl/pump.cpp"));
  CHECK_FALSE(sb.resolve_read("stream.pump:api/../impl/pump.cpp"));
}

TEST_CASE("sandbox: absolute paths are refused") {
  test::FixtureCopy p("wavkit");
  agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
  auto r = sb.resolve_read("/etc/passwd");
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "absolute paths are not allowed"));
  CHECK_FALSE(sb.resolve_write((p / "blocks/stream/ring/impl/x.cpp").string()));
  CHECK_FALSE(sb.resolve_read("~/.ssh/id_rsa"));
}

TEST_CASE("sandbox: symlinks pointing outside are refused") {
  test::FixtureCopy p("wavkit");
  std::filesystem::create_directory_symlink("/etc", p / "blocks/stream/ring/impl/etc");
  std::filesystem::create_symlink(p / "blocks/stream/pump/impl/pump.cpp", p / "blocks/stream/ring/impl/peek.cpp");
  agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
  auto r = sb.resolve_read("impl/etc/passwd");
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "outside the sandbox"));
  CHECK_FALSE(sb.resolve_read("impl/peek.cpp"));
  CHECK_FALSE(sb.resolve_write("impl/peek.cpp"));
}

TEST_CASE("sandbox: writing api/ is refused with a hint to report needs_contract_change") {
  test::FixtureCopy p("wavkit");
  agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
  auto r = sb.resolve_write("api/ring.hpp");
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "report status=needs_contract_change"));
}

TEST_CASE("sandbox: other blocks' impl/ and unlinked api/ are not readable") {
  test::FixtureCopy p("wavkit");
  agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
  CHECK_FALSE(sb.resolve_read("stream.pump:impl/pump.cpp"));
  auto r = sb.resolve_read("dsp.gain:api/gain.hpp");
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "not linked"));
}

TEST_CASE("sandbox: the tester never reads impl/ and writes only tests/contract/ and mocks/") {
  test::FixtureCopy p("wavkit");
  agents::Sandbox sb(wav(p), "stream.pump", agents::Role::Tester);
  auto r = sb.resolve_read("impl/pump.cpp");
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "impl/ is not readable"));
  CHECK(sb.resolve_read("api/pump.hpp"));
  CHECK(sb.resolve_read("mocks/io_reader_mock.hpp"));
  CHECK(sb.resolve_read("io.reader:api/reader.hpp"));
  CHECK(sb.resolve_write("tests/contract/pump_contract.cpp"));
  CHECK(sb.resolve_write("mocks/extra.hpp"));
  CHECK_FALSE(sb.resolve_write("impl/pump.cpp"));
  CHECK_FALSE(sb.resolve_write("tests/unit/x.cpp"));
}

TEST_CASE("leaf tools: read, list, grep, edit with unique match, write refusals") {
  test::FixtureCopy p("wavkit");
  test::FakeTests tests;
  agents::LeafToolHost host(agents::Sandbox(wav(p), "stream.ring", agents::Role::Worker), agents::Role::Worker, tests, {"stream.ring"});
  auto read = host.call({"1", "read_file", R"({"path":"impl/ring.cpp","offset":1,"limit":2})"});
  CHECK_FALSE(read.is_error);
  CHECK(core::contains(read.content, "#include \"ring.hpp\""));
  // Lines keep their indentation: they are copied into edit_file's `old`.
  auto file = test::read(p / "blocks/stream/ring/impl/ring.cpp");
  auto indented = file.find("\n  ");
  REQUIRE(indented != std::string::npos);
  int line_no = 1 + static_cast<int>(std::count(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(indented), '\n')) + 1;
  auto part = host.call({"1b", "read_file", fmt::format(R"({{"path":"impl/ring.cpp","offset":{},"limit":1}})", line_no)});
  CHECK(part.content.starts_with("  "));
  auto list = host.call({"2", "list_dir", R"({"path":"."})"});
  CHECK(core::contains(list.content, "impl/"));
  auto grep = host.call({"3", "grep", R"({"pattern":"mask_","path":"impl"})"});
  CHECK(core::contains(grep.content, "impl/ring.cpp:"));
  auto dup = host.call({"4", "edit_file", R"({"path":"impl/ring.cpp","old":"mask_","new":"m_"})"});
  CHECK(dup.is_error);
  CHECK(core::contains(dup.content, "not unique"));
  auto ed = host.call({"5", "edit_file", R"({"path":"impl/ring.cpp","old":"explicit Ring(","new":"explicit Ring /*x*/ ("})"});
  CHECK_FALSE(ed.is_error);
  auto api = host.call({"6", "write_file", R"({"path":"api/ring.hpp","content":"x"})"});
  CHECK(api.is_error);
  auto out = host.call({"7", "read_file", R"({"path":"/etc/passwd"})"});
  CHECK(out.is_error);
  auto rep = host.call({"8", "report", R"({"status":"fixed"})"});
  CHECK(rep.terminal);
  CHECK(host.reads().size() >= 2);
}

TEST_CASE("sandbox: api_shape ignores values of constants and comments, not declarations") {
  const std::string base =
      "#pragma once\n// tunables\nnamespace config {\ninline constexpr float kGapH = 185.0f;  // gap\n"
      "inline constexpr int kFps = 60;\ninline constexpr Rgb kSky{10, 20, 30};\n"
      "inline constexpr float kGroundY = static_cast<float>(kScreenH) - kGroundH;\n"
      "constexpr float twice(float x) { return x * 2; }\nvoid reset();\n}\n";
  auto shape = agents::api_shape(base);
  auto tuned = base;
  tuned.replace(tuned.find("185.0f"), 6, "222.0f");
  tuned.replace(tuned.find("{10, 20, 30}"), 12, "{1, 2, 3}");
  tuned.replace(tuned.find("// gap"), 6, "// wider gap");
  CHECK(agents::api_shape(tuned) == shape);
  auto renamed = base;
  renamed.replace(renamed.find("kFps"), 4, "kFrames");
  CHECK(agents::api_shape(renamed) != shape);
  auto added = base + "void start_slow();\n";
  CHECK(agents::api_shape(added) != shape);
  auto body = base;
  body.replace(body.find("x * 2"), 5, "x * 3");
  CHECK(agents::api_shape(body) != shape);  // a constexpr function body is code, not a value
}

TEST_CASE("leaf tools: a frozen api/ takes value changes only, an unfrozen one takes any edit") {
  test::FixtureCopy p("wavkit");
  test::FakeTests tests;
  test::write(p / "blocks/stream/ring/api/limits.hpp",
              "#pragma once\nnamespace wk {\ninline constexpr int kMaxFrames = 4096;\n}\n");
  {
    agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
    sb.set_api_access(agents::ApiAccess::ValuesOnly);
    agents::LeafToolHost host(std::move(sb), agents::Role::Worker, tests, {"stream.ring"});
    auto value = host.call({"1", "edit_file", R"({"path":"api/limits.hpp","old":"4096","new":"8192"})"});
    CHECK_FALSE(value.is_error);
    CHECK(host.api_written());
    CHECK(core::contains(test::read(p / "blocks/stream/ring/api/limits.hpp"), "8192"));
    auto decl = host.call({"2", "edit_file", R"({"path":"api/ring.hpp","old":"  [[nodiscard]] virtual std::size_t fill() const = 0;","new":"  [[nodiscard]] virtual std::size_t fill() const = 0;\n  virtual void reset() = 0;"})"});
    CHECK(decl.is_error);
    CHECK(core::contains(decl.content, "only values of its constants may change"));
    CHECK_FALSE(core::contains(test::read(p / "blocks/stream/ring/api/ring.hpp"), "reset"));
    auto fresh = host.call({"3", "write_file", R"({"path":"api/extra.hpp","content":"#pragma once\nvoid x();\n"})"});
    CHECK(fresh.is_error);
  }
  {
    agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
    sb.set_api_access(agents::ApiAccess::Full);
    agents::LeafToolHost host(std::move(sb), agents::Role::Worker, tests, {"stream.ring"});
    auto decl = host.call({"4", "edit_file", R"({"path":"api/ring.hpp","old":"  [[nodiscard]] virtual std::size_t fill() const = 0;","new":"  [[nodiscard]] virtual std::size_t fill() const = 0;\n  virtual void reset() = 0;"})"});
    CHECK_FALSE(decl.is_error);
    CHECK(host.api_written());
  }
  // The tester never writes api/, whatever the access.
  agents::Sandbox tester(wav(p), "stream.ring", agents::Role::Tester);
  tester.set_api_access(agents::ApiAccess::Full);
  CHECK_FALSE(tester.resolve_write("api/ring.hpp"));
}

TEST_CASE("sandbox: a frozen api/ may gain constexpr constants, removed or changed declarations stay refused") {
  const std::string base =
      "#pragma once\nnamespace config {\ninline constexpr float kGapH = 185.0f;\ninline constexpr int kFps = 60;\n"
      "struct Rgb {\n  unsigned char r, g, b;\n};\nvoid reset();\n}\n";
  auto added = base;
  added.replace(added.find("inline constexpr int kFps"), 0, "inline constexpr float kSlowFactor = 0.5f;  // Slow\n");
  CHECK(agents::api_values_or_new_constants(base, added));
  auto tuned_and_added = added;
  tuned_and_added.replace(tuned_and_added.find("185.0f"), 6, "222.0f");
  CHECK(agents::api_values_or_new_constants(base, tuned_and_added));
  auto removed = base;
  removed.erase(removed.find("inline constexpr int kFps = 60;\n"), 32);
  CHECK_FALSE(agents::api_values_or_new_constants(base, removed));
  auto fn = base;
  fn.replace(fn.find("void reset();"), 0, "void start_slow();\n");
  CHECK_FALSE(agents::api_values_or_new_constants(base, fn));
  auto constexpr_fn = base;
  constexpr_fn.replace(constexpr_fn.find("void reset();"), 0, "constexpr float half(float x) { return x / 2; }\n");
  CHECK_FALSE(agents::api_values_or_new_constants(base, constexpr_fn));
  auto field = base;
  field.replace(field.find("r, g, b;"), 8, "r, g, b, a;");
  CHECK_FALSE(agents::api_values_or_new_constants(base, field));
}

TEST_CASE("sandbox: a worker reads the api/ of the blocks that use its block, without them in its neighbors") {
  test::FixtureCopy p("wavkit");
  agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
  // dsp.chain takes frames from the ring (data link): it uses the ring's api.
  CHECK(std::find(sb.user_blocks().begin(), sb.user_blocks().end(), "dsp.chain") != sb.user_blocks().end());
  auto neighbors = sb.neighbor_blocks();
  CHECK(std::find(neighbors.begin(), neighbors.end(), "dsp.chain") == neighbors.end());
  CHECK(sb.resolve_read("dsp.chain:api/chain.hpp"));
  CHECK_FALSE(sb.resolve_write("dsp.chain:api/chain.hpp"));
  CHECK_FALSE(sb.resolve_read("dsp.chain:impl/chain.cpp"));
  // The tester gets no such reads.
  agents::Sandbox tester(wav(p), "stream.ring", agents::Role::Tester);
  CHECK(tester.user_blocks().empty());
}

// free-AI log: agents that only heard "not linked" / "cannot read" / "not a folder" guessed for
// 20 of 25 steps ("game:", "core:", "common:" for a header of shared.types).
TEST_CASE("sandbox: a refused block names what is readable, a group its readable blocks, a wrong name its absence") {
  test::FixtureCopy p("wavkit");
  agents::Sandbox sb(wav(p), "stream.ring", agents::Role::Worker);
  auto group = sb.resolve_read("stream:api/stream.hpp");
  REQUIRE_FALSE(group);
  CHECK(core::contains(group.error().message, "stream is a group, which has no api/; its blocks readable here: stream.pump"));
  CHECK(core::contains(group.error().message, "readable are your own folder and the api/ of: "));
  CHECK(core::contains(group.error().message, "shared.types"));
  auto none = sb.resolve_read("game:api/shared.hpp");
  REQUIRE_FALSE(none);
  CHECK(core::contains(none.error().message, "there is no block game"));
  auto other = sb.resolve_read("dsp.gain:api/gain.hpp");
  REQUIRE_FALSE(other);
  CHECK(core::contains(other.error().message, "block dsp.gain is not linked to stream.ring"));
}

TEST_CASE("leaf tools: a missing file or folder says what is there instead") {
  test::FixtureCopy p("wavkit");
  test::FakeTests tests;
  agents::LeafToolHost host(agents::Sandbox(wav(p), "stream.ring", agents::Role::Worker), agents::Role::Worker, tests, {"stream.ring"});
  auto file = host.call({"1", "read_file", R"({"path":"stream.pump:api/shared.hpp"})"});
  CHECK(file.is_error);
  CHECK(core::contains(file.content, "no such file: stream.pump:api/shared.hpp; stream.pump:api has: pump.hpp"));
  auto dir = host.call({"2", "list_dir", R"({"path":"tests/integration"})"});
  CHECK(dir.is_error);
  CHECK(core::contains(dir.content, "no such folder: tests/integration; tests has: contract/, unit/"));
  auto notdir = host.call({"3", "list_dir", R"({"path":"block.ksl"})"});
  CHECK(core::contains(notdir.content, "block.ksl is a file, not a folder"));
  // The own folder of an integration tester exists only after its first file.
  agents::LeafToolHost integ(agents::Sandbox(wav(p), "stream", agents::Role::Tester), agents::Role::Tester, tests, {"stream"});
  auto empty = integ.call({"4", "list_dir", R"({"path":"."})"});
  CHECK_FALSE(empty.is_error);
  CHECK(core::contains(empty.content, "empty: nothing is written here yet"));
}

TEST_CASE("leaf tools: a build broken only in others' files says so, and no tests yet says how tests are built") {
  test::FixtureCopy p("wavkit");
  test::FakeTests tests;
  std::string summary = "blocks/dsp/gain/impl/gain.cpp:3:1: error: 'x' was not declared\n"
                        "/usr/include/c++/16/bits/unique_ptr.h:1105:30: error: invalid new-expression\n";
  tests.fn = [&](const std::vector<std::string>&) { return agents::TestRun{false, false, summary, {}, "build failed\n" + summary}; };
  agents::LeafToolHost host(agents::Sandbox(wav(p), "stream.ring", agents::Role::Worker), agents::Role::Worker, tests, {"stream.ring"});
  auto foreign = host.call({"1", "test", "{}"});
  CHECK(core::contains(foreign.content, "None of these errors is in a file you may change (blocks/dsp/gain/impl/gain.cpp)"));
  summary = "blocks/stream/ring/impl/ring.cpp:9:1: error: 'y' was not declared\n" + summary;
  auto own = host.call({"2", "test", "{}"});
  CHECK_FALSE(core::contains(own.content, "None of these errors"));
  // The tester's own impl/ is not its file either.
  agents::LeafToolHost tester(agents::Sandbox(wav(p), "stream.ring", agents::Role::Tester), agents::Role::Tester, tests, {"stream.ring"});
  CHECK(core::contains(tester.call({"3", "test", "{}"}).content, "None of these errors is in a file you may change"));
  summary = "CMake Error at CMakeLists.txt:149 (add_executable): No SOURCES given to target: x_tests\n";
  CHECK(core::contains(host.call({"4", "test", "{}"}).content, "CMakeLists.txt is generated by Karkas"));
  tests.fn = [](const std::vector<std::string>&) { return agents::TestRun{true, false, "", {}, "tests none found\n"}; };
  auto none = host.call({"5", "test", "{}"});
  CHECK(core::contains(none.content, "No test of stream.ring exists yet"));
  CHECK(core::contains(none.content, "doctest"));
}
