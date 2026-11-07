#include <doctest/doctest.h>

#include <cstdlib>

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
