#include <doctest/doctest.h>

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

TEST_CASE("paths: is_within rejects escapes") {
  kk::test::TempDir d{"paths"};
  kk::test::write(d / "blk/impl/a.cpp", "x");
  CHECK(is_within(d / "blk", d / "blk/impl/a.cpp"));
  CHECK(is_within(d / "blk", d / "blk/impl/../impl/new.cpp"));
  CHECK_FALSE(is_within(d / "blk", d / "blk/../other.cpp"));
  CHECK_FALSE(is_within(d / "blk", "/etc/passwd"));
  std::filesystem::create_directory_symlink("/tmp", d / "blk/link");
  CHECK_FALSE(is_within(d / "blk", d / "blk/link/x"));
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
