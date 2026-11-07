#include <doctest/doctest.h>

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
