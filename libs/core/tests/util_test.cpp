#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <thread>

#include "kk/core/cancel.hpp"
#include "kk/core/config.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"

using namespace kk::core;

TEST_CASE("text: identifiers and paths (4.2)") {
  CHECK(is_ident("ring"));
  CHECK(is_ident("a1_b"));
  CHECK_FALSE(is_ident("Ring"));
  CHECK_FALSE(is_ident("1a"));
  CHECK_FALSE(is_ident(""));
  CHECK(is_block_path("stream.ring"));
  CHECK_FALSE(is_block_path("stream..ring"));
  CHECK_FALSE(is_block_path(".ring"));
  CHECK(is_record_id("T-0042"));
  CHECK(is_record_id("C-3"));
  CHECK_FALSE(is_record_id("X-1"));
  CHECK_FALSE(is_record_id("T-"));
}

TEST_CASE("text: token estimate is in a sane range") {
  std::string_view s = "stream.ring  leaf  lock-free single-producer single-consumer ring of audio frames";
  auto t = estimate_tokens(s);
  CHECK(t >= 15);
  CHECK(t <= 40);
  CHECK(estimate_tokens("") == 0);
}

namespace {
// Strict check of well-formed UTF-8 sequences (lengths only; enough for these tests).
bool valid_utf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = c < 0x80u ? 1 : (c & 0xE0u) == 0xC0u ? 2 : (c & 0xF0u) == 0xE0u ? 3 : (c & 0xF8u) == 0xF0u ? 4 : 0;
    if (len == 0 || i + len > s.size()) return false;
    for (std::size_t k = 1; k < len; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u) return false;
    }
    i += len;
  }
  return true;
}
}  // namespace

TEST_CASE("text: truncate_utf8 never cuts a multi-byte code point") {
  CHECK(truncate_utf8("", 0) == "");
  CHECK(truncate_utf8("abc", 10) == "abc");
  CHECK(truncate_utf8("abc", 2) == "ab");
  // "абвгд": five 2-byte code points. A cut inside "б" (3 bytes) steps back to its start.
  std::string cyr = "абвгд";
  CHECK(truncate_utf8(cyr, 3) == "а");
  // A cut that lands on a lead byte is already a boundary: nothing is dropped.
  CHECK(truncate_utf8(cyr, 4) == "аб");
  // ASCII, 2-, 3- and 4-byte code points: every cut is a valid prefix, as long as possible.
  std::string mixed = "a\xD0\xB0\xE2\x82\xAC\xF0\x9F\x98\x80z";  // a а € 😀 z
  for (std::size_t n = 0; n <= mixed.size() + 1; ++n) {
    auto cut = truncate_utf8(mixed, n);
    CAPTURE(n);
    CHECK(cut.size() <= n);
    CHECK(std::string_view(mixed).starts_with(cut));
    CHECK(valid_utf8(cut));
    CHECK(cut.size() + 3 >= std::min(n, mixed.size()));  // drops at most one incomplete code point
  }
  CHECK(truncate_utf8(mixed, 5) == "a\xD0\xB0");  // inside € (bytes 3..5)
  CHECK(truncate_utf8(mixed, 9) == "a\xD0\xB0\xE2\x82\xAC");  // inside 😀 (bytes 6..9)
}

TEST_CASE("paths: is_within rejects escapes") {
  kk::test::TempDir d{"paths"};
  kk::test::write(d / "blk/impl/a.cpp", "x");
  CHECK(is_within(d / "blk", d / "blk/impl/a.cpp"));
  CHECK(is_within(d / "blk", d / "blk/impl/../impl/new.cpp"));
  CHECK_FALSE(is_within(d / "blk", d / "blk/../other.cpp"));
  CHECK_FALSE(is_within(d / "blk", "/etc/passwd"));
#ifndef _WIN32  // a symlink needs a privilege on Windows (docs/adr/0046)
  std::filesystem::create_directory_symlink("/tmp", d / "blk/link");
  CHECK_FALSE(is_within(d / "blk", d / "blk/link/x"));
#endif
}

TEST_CASE("paths: atomic write and target names") {
  kk::test::TempDir d{"write"};
  REQUIRE(write_file(d / "a/b/c.txt", "hello"));
  CHECK(read_file(d / "a/b/c.txt").value() == "hello");
  CHECK(target_name("stream.ring") == "stream_ring");
  CHECK(is_code_file("x/impl/a.cpp"));
  CHECK_FALSE(is_code_file("x/block.ksl"));
}

TEST_CASE("config: appendix E example parses") {
  Config cfg;
  auto r = apply_config_text(cfg, R"([project]
lang      = "cpp"
build_dir = "out"
cmake_args = "-DBUILD_TESTS=ON -DX=1"
[test]
command = "build/test/blobbytest"
[models.strong]
provider = "anthropic"
model    = "claude-opus-5-5"
[models.local]
provider = "openai_compat"
base_url = "http://localhost:11434/v1"
model    = "qwen"
[roles]
worker = "auto"
[routing]
cheap_max = 2
[limits]
max_steps_worker = 30
[cache]
ttl = "1h"
[prices]
"claude-opus-5-5" = { input = 15.0, output = 75.0 }
cache_read = 0.2
[gui]
editor_command = "kitty -e nvim +{line} {file}"
)",
                             "karkas.toml");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(cfg.build_dir == "out");
  CHECK(cfg.cmake_args == "-DBUILD_TESTS=ON -DX=1");
  CHECK(cfg.test_command == "build/test/blobbytest");
  CHECK(cfg.tier("strong")->model == "claude-opus-5-5");
  CHECK(cfg.tier("local")->provider == "openai_compat");
  CHECK(cfg.cheap_max == 2);
  CHECK(cfg.max_steps_worker == 30);
  CHECK(cfg.cache_ttl == "1h");
  CHECK(cfg.prices["claude-opus-5-5"].output == doctest::Approx(75.0));
  CHECK(cfg.cache_read == doctest::Approx(0.2));
  CHECK(cfg.roles["worker"] == "auto");
  CHECK(cfg.editor_command == "kitty -e nvim +{line} {file}");
}

TEST_CASE("config: syntax errors carry file and line") {
  Config cfg;
  auto r = apply_config_text(cfg, "[limits]\nmax_steps_worker = = 3\n", "karkas.toml");
  REQUIRE_FALSE(r);
  CHECK(r.error().loc.file == "karkas.toml");
  CHECK(r.error().loc.line == 2);
  auto bad = apply_config_text(cfg, "[cache]\nttl = \"2h\"\n", "k.toml");
  CHECK_FALSE(bad);
}

TEST_CASE("config: models pinned per level and per block (ADR 0011)") {
  Config cfg;
  auto r = apply_config_text(cfg, R"([models.local]
provider    = "openai_compat"
model       = "qwen3-coder"
base_url    = "http://localhost:11434/v1"
api_key_env = "LOCAL_KEY"

[levels]
0 = "strong"
2 = "cheap"

[blocks]
"game" = "local"
"game.flow.timer" = "medium"
)", "karkas.toml");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(cfg.tier("local")->key_env() == "LOCAL_KEY");
  CHECK(ModelTier{}.key_env() == "ANTHROPIC_API_KEY");
  CHECK(block_level("") == 0);
  CHECK(block_level("root") == 0);
  CHECK(block_level("world.bird") == 2);
  CHECK(cfg.pin("root").tier == "strong");
  CHECK(cfg.pin("root").by == "level 0");
  CHECK(cfg.pin("world").tier.empty());  // level 1: nothing pinned
  CHECK(cfg.pin("world.bird").tier == "cheap");
  CHECK(cfg.pin("game").by == "block game");
  CHECK(cfg.pin("game.flow").tier == "local");  // inside game: the block beats the level
  CHECK(cfg.pin("game.flow.timer").tier == "medium");  // the longest path wins
  CHECK(cfg.pin("gamepad").tier.empty());  // a prefix of the name is not an ancestor
  Config bad;
  CHECK_FALSE(apply_config_text(bad, "[levels]\ntop = \"strong\"\n", "k.toml"));
  CHECK_FALSE(apply_config_text(bad, "[blocks]\n\"Game\" = \"strong\"\n", "k.toml"));
  // [levels] pin the deciding roles; the work on a leaf goes by [roles] and routing unless
  // [routing] levels_for = "all" (ADR 0026). [blocks] pins every role.
  CHECK(cfg.pin("world.bird", "worker").tier.empty());
  CHECK(cfg.pin("world.bird", "ask").tier == "cheap");
  CHECK(cfg.pin("game.flow.timer", "tester").tier == "medium");
  Config all;
  REQUIRE(apply_config_text(all, "[levels]\n2 = \"cheap\"\n[routing]\nlevels_for = \"all\"\n", "k.toml"));
  CHECK(all.pin("world.bird", "worker").tier == "cheap");
  CHECK_FALSE(apply_config_text(bad, "[routing]\nlevels_for = \"leaves\"\n", "k.toml"));
}

TEST_CASE("config: escalation chain and reasoning effort per role (ADR 0020)") {
  Config cfg;
  CHECK(cfg.escalate.empty());
  auto r = apply_config_text(cfg, R"([routing]
escalate = ["deepseek_flash", "deepseek"]

[effort]
orchestrator = "low"
navigator    = "low"
)", "karkas.toml");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(cfg.escalate == std::vector<std::string>{"deepseek_flash", "deepseek"});
  CHECK(cfg.role_effort.at("orchestrator") == "low");
  CHECK(cfg.role_effort.at("navigator") == "low");
  CHECK_FALSE(cfg.role_effort.contains("worker"));
  CHECK_FALSE(cfg.auto_approve);
  REQUIRE(apply_config_text(cfg, "[agents]\nauto_approve = true\n", "k.toml"));
  CHECK(cfg.auto_approve);
  REQUIRE(apply_config_text(cfg, "[agents]\nauto_approve = \"false\"\n", "k.toml"));
  CHECK_FALSE(cfg.auto_approve);
}

TEST_CASE("config: set_config_value edits one key and keeps the rest") {
  std::string text = "# my settings\n[models.strong]\nmodel = \"claude-opus-5-5\"  # best\n\n[roles]\nworker = \"auto\"\n";
  auto a = set_config_value(text, "levels", "1", "strong");
  REQUIRE(a);
  CHECK(*a == text + "\n[levels]\n1 = \"strong\"\n");
  auto b = set_config_value(*a, "blocks", "world.pickups", "cheap");
  REQUIRE(b);
  CHECK(contains(*b, "[blocks]\n\"world.pickups\" = \"cheap\"\n"));
  auto c = set_config_value(*b, "roles", "tester", "medium");
  REQUIRE(c);
  CHECK(contains(*c, "[roles]\nworker = \"auto\"\ntester = \"medium\"\n"));
  auto d = set_config_value(*c, "blocks", "world.pickups", "strong");
  REQUIRE(d);
  CHECK(contains(*d, "\"world.pickups\" = \"strong\""));
  CHECK_FALSE(contains(*d, "cheap"));
  auto e = set_config_value(*d, "blocks", "world.pickups", std::nullopt);
  REQUIRE(e);
  CHECK_FALSE(contains(*e, "world.pickups"));
  CHECK(contains(*e, "# my settings\n"));
  CHECK(contains(*e, "# best"));
  auto f = set_config_value(*e, "models.local", "model", "say \"hi\"");
  REQUIRE(f);
  Config cfg;
  REQUIRE(apply_config_text(cfg, *f, "k.toml"));
  CHECK(cfg.tier("local")->model == "say \"hi\"");
  auto g = remove_config_section(*f, "models.local");
  CHECK_FALSE(contains(g, "local"));
  CHECK(contains(g, "[roles]"));
  CHECK_FALSE(set_config_value(text, "levels", "top", "strong"));  // invalid result is refused
}

TEST_CASE("config: [project] run_command, written with quotes and read back (ADR 0037)") {
  auto t = set_config_value("[project]\nbuild_dir = \"build\"\n", "project", "run_command", R"(build/game --name "Player 1")");
  REQUIRE(t);
  Config cfg;
  REQUIRE(apply_config_text(cfg, *t, "k.toml"));
  CHECK(cfg.run_command == R"(build/game --name "Player 1")");
  CHECK(cfg.build_dir == "build");
  CHECK(Config{}.run_command.empty());
}

TEST_CASE("text: to_ident makes KSL identifiers from C++ and file names") {
  CHECK(to_ident("ResolveCollision") == "resolve_collision");
  CHECK(to_ident("Update") == "update");
  CHECK(to_ident("getPosition") == "get_position");
  CHECK(to_ident("HTTPServer") == "http_server");
  CHECK(to_ident("ScreenToWorld") == "screen_to_world");
  CHECK(to_ident("audio_manager") == "audio_manager");
  CHECK(to_ident("2d-math") == "x2d_math");
  CHECK(to_ident("glibc_compat") == "glibc_compat");
  CHECK(to_ident("Vec2") == "vec2");
  CHECK(to_ident("__x__") == "x");
  CHECK(to_ident("") == "");
  CHECK(is_ident(to_ident("Draw_UI")));
}

TEST_CASE("cancel: process flag and per-thread scopes (ADR 0019)") {
  reset_cancel();
  CHECK_FALSE(cancelled());
  bool stop = false;
  {
    CancelScope outer([&] { return stop; });
    CHECK_FALSE(cancelled());
    {
      CancelScope inner([] { return false; });
      stop = true;
      CHECK(cancelled());  // an outer scope counts inside an inner one
    }
    stop = false;
    CHECK_FALSE(cancelled());
    bool other_thread = true;
    std::thread([&] {
      stop = true;
      other_thread = cancelled();  // scopes belong to their thread
    }).join();
    CHECK_FALSE(other_thread);
    CHECK(cancelled());
  }
  CHECK_FALSE(cancelled());
  request_cancel();
  CHECK(cancel_requested());
  CHECK(cancelled());
  reset_cancel();
  CHECK_FALSE(cancelled());
}

TEST_CASE("config: [agents] mode picks the model maps of that mode, both are kept (ADR 0024)") {
  Config c;
  REQUIRE(apply_config_text(c, R"(
[levels]
1 = "deepseek_flash"
[roles]
orchestrator = "deepseek"
[routing]
escalate = ["deepseek_flash", "deepseek"]
[mcp.levels]
1 = "cheap"
[mcp.roles]
orchestrator = "strong"
[mcp.blocks]
"game.flow" = "medium"
[mcp.routing]
escalate = ["cheap", "medium", "strong"]
)",
                            "karkas.toml"));
  CHECK(c.agents_mode == "api");
  CHECK(c.levels.at(1) == "deepseek_flash");
  CHECK(c.roles.at("orchestrator") == "deepseek");
  CHECK(c.escalate == std::vector<std::string>{"deepseek_flash", "deepseek"});
  CHECK(c.map_section("levels") == "levels");
  CHECK(c.mcp_maps.levels.at(1) == "cheap");
  // The project file switches the mode: the MCP maps become the active ones.
  REQUIRE(apply_config_text(c, "[agents]\nmode = \"mcp\"\n", "karkas.toml"));
  CHECK(c.mcp());
  CHECK(c.levels.at(1) == "cheap");
  CHECK(c.roles.at("orchestrator") == "strong");
  CHECK(c.roles.at("navigator") == "medium");  // a role the MCP maps leave out keeps its default
  CHECK(c.block_tiers.at("game.flow") == "medium");
  CHECK(c.escalate == std::vector<std::string>{"cheap", "medium", "strong"});
  CHECK(c.api_maps.levels.at(1) == "deepseek_flash");  // the API maps stay for the switch back
  CHECK(c.map_section("levels") == "mcp.levels");
  CHECK(c.pin("game.flow").tier == "medium");
  Config bad;
  CHECK_FALSE(apply_config_text(bad, "[agents]\nmode = \"sampling\"\n", "karkas.toml"));
  // The GUI writes the maps of a mode into its own section.
  auto text = set_config_value("[agents]\nmode = \"mcp\"\n", "mcp.levels", "2", std::string("cheap"));
  REQUIRE(text);
  CHECK(contains(*text, "[mcp.levels]\n2 = \"cheap\""));
}
