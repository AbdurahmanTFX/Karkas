#include <doctest/doctest.h>

#include <algorithm>
#include <nlohmann/json.hpp>

#include "kk/code/init.hpp"
#include "kk/core/config.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"

using namespace kk;
namespace fs = std::filesystem;

TEST_CASE("init: project ids from folder names") {
  CHECK(code::project_id_from("Flappy-Birds") == "flappy_birds");
  CHECK(code::project_id_from("wavkit") == "wavkit");
  CHECK(code::project_id_from("2048") == "");
  CHECK(code::project_id_from("") == "");
}

TEST_CASE("init: a new folder gets the project frame") {
  test::TempDir tmp;
  auto root = tmp.path() / "My Game";
  auto r = code::init_project(root, "a tiny arcade game");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(r->name == "my_game");
  CHECK_FALSE(r->existing_code);
  REQUIRE(r->written.size() >= 5);
  CHECK(std::vector<std::string>(r->written.begin(), r->written.begin() + 5) ==
        std::vector<std::string>{"project.ksl", "karkas.toml", "cmake/user.cmake", ".gitignore", "CLAUDE.md"});
  // Subagents of the default tiers (more if the user's config adds Claude tiers).
  for (const char* tier : {"strong", "medium", "cheap"}) {
    auto f = std::string(".claude/agents/karkas-") + tier + ".md";
    CHECK_MESSAGE(std::ranges::find(r->written, f) != r->written.end(), f);
  }
  CHECK(core::contains(test::read(root / "CLAUDE.md"), "kk_propose_change"));
  CHECK(fs::is_directory(root / "blocks"));
  CHECK(fs::is_directory(root / ".karkas"));
  auto s = core::load_schema(root);
  REQUIRE_MESSAGE(s, (s ? "" : s.error().str()));
  CHECK(s->project.name == "my_game");
  CHECK(s->project.idea.value == "a tiny arcade game");
  CHECK(core::load_config(root));
  // A second init in the same folder is refused.
  CHECK_FALSE(code::init_project(root));
}

TEST_CASE("init: a folder with code keeps its standard and gets no rules") {
  test::TempDir tmp;
  auto root = tmp.path() / "game";
  fs::create_directories(root / "src");
  test::write(root / "src/main.cpp", "int main() { return 0; }\n");
  test::write(root / "CMakeLists.txt", "set(CMAKE_CXX_STANDARD 17)\n");
  auto r = code::init_project(root);
  REQUIRE(r);
  CHECK(r->existing_code);
  auto text = test::read(root / "project.ksl");
  CHECK(core::contains(text, "std     c++17"));
  CHECK_FALSE(core::contains(text, "rule"));
  CHECK(test::read(root / "src/main.cpp") == "int main() { return 0; }\n");
}

TEST_CASE("init: the agent guide is a managed block of CLAUDE.md") {
  test::TempDir tmp;
  auto root = tmp.path();
  test::write(root / "CLAUDE.md", "# My notes\nbuild with make\n");
  auto a = code::write_agent_guide(root);
  REQUIRE(a);
  CHECK(*a == "updated");
  auto text = test::read(root / "CLAUDE.md");
  CHECK(text.starts_with("# My notes\nbuild with make\n\n<!-- karkas:begin"));
  CHECK(core::contains(text, "Schema first"));
  CHECK(text.ends_with("<!-- karkas:end -->\n"));
  // Again: nothing changes; an edited block is replaced, the user's text stays.
  CHECK(*code::write_agent_guide(root) == "unchanged");
  auto edited = core::replace_all(text, "Schema first", "old text");
  test::write(root / "CLAUDE.md", edited + "## After\nmore notes\n");
  CHECK(*code::write_agent_guide(root) == "updated");
  auto again = test::read(root / "CLAUDE.md");
  CHECK(again == text + "## After\nmore notes\n");
}

TEST_CASE("init: Claude Code subagents follow the model tiers (ADR 0013)") {
  test::TempDir tmp;
  auto root = tmp.path();
  core::Config cfg;
  auto tier = [](std::string provider, std::string model) {
    core::ModelTier t;
    t.provider = std::move(provider);
    t.model = std::move(model);
    return t;
  };
  cfg.models = {{"strong", tier("anthropic", "claude-opus-5-5")},
                {"cheap", tier("anthropic", "claude-haiku-5-5")},
                {"Fast_One", tier("anthropic", "claude-sonnet-5-5")},
                {"local", tier("openai_compat", "qwen3-coder")}};
  auto r = code::write_claude_agents(root, cfg);
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(r->written == std::vector<std::string>{".claude/agents/karkas-fast-one.md", ".claude/agents/karkas-cheap.md",
                                               ".claude/agents/karkas-strong.md"});
  auto cheap = test::read(root / ".claude/agents/karkas-cheap.md");
  CHECK(cheap.starts_with("---\nname: karkas-cheap\n"));
  CHECK(core::contains(cheap, "\nmodel: claude-haiku-5-5\n---\n"));
  CHECK(core::contains(cheap, "karkas:generated"));
  CHECK(core::contains(cheap, "kk_model_for names karkas-cheap"));
  CHECK_FALSE(fs::exists(root / ".claude/agents/karkas-local.md"));  // Claude Code cannot run it
  // Again: nothing changes. A hand-written file is kept.
  CHECK(code::write_claude_agents(root, cfg)->unchanged.size() == 3);
  test::write(root / ".claude/agents/karkas-strong.md", "---\nname: karkas-strong\nmodel: opus\n---\nmine\n");
  // The cheap tier changes model, Fast_One is gone: rewritten and removed; the user's file stays.
  cfg.models["cheap"].model = "claude-sonnet-5-5";
  cfg.models.erase("Fast_One");
  auto again = code::write_claude_agents(root, cfg);
  REQUIRE(again);
  CHECK(again->written == std::vector<std::string>{".claude/agents/karkas-cheap.md"});
  CHECK(again->skipped == std::vector<std::string>{".claude/agents/karkas-strong.md"});
  CHECK(again->removed == std::vector<std::string>{".claude/agents/karkas-fast-one.md"});
  CHECK(core::contains(test::read(root / ".claude/agents/karkas-cheap.md"), "model: claude-sonnet-5-5"));
  CHECK(test::read(root / ".claude/agents/karkas-strong.md").ends_with("mine\n"));
  CHECK(core::subagent_name("Fast_One") == "karkas-fast-one");
}

TEST_CASE("init: in mode mcp a subagent only serves turns: the two turn tools and a short prompt (token-cost-notes 5)") {
  test::TempDir d;
  const auto& root = d.path();
  core::Config cfg;
  core::ModelTier t;
  t.provider = "anthropic";
  t.model = "claude-haiku-5-5";
  cfg.models["cheap"] = t;
  cfg.agents_mode = "mcp";
  auto r = code::write_claude_agents(root, cfg);
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto cheap = test::read(root / ".claude/agents/karkas-cheap.md");
  CHECK(cheap.starts_with("---\nname: karkas-cheap\n"));
  CHECK(core::contains(cheap, "\ntools: mcp__karkas__kk_agent_next, mcp__karkas__kk_agent_act, ToolSearch\n"));
  CHECK(core::contains(cheap, "serve the\n  next run only when it names you (karkas-cheap)"));
  CHECK_FALSE(core::contains(cheap, "A block and a task"));
  CHECK(cheap.size() < 2200);
  // Back to mode api: the subagent works on blocks again, with every tool.
  cfg.agents_mode = "api";
  REQUIRE(code::write_claude_agents(root, cfg)->written.size() == 1);
  auto api = test::read(root / ".claude/agents/karkas-cheap.md");
  CHECK(core::contains(api, "A block and a task"));
  CHECK_FALSE(core::contains(api, "\ntools:"));
}

TEST_CASE("init: the subagents as JSON for claude --agents") {
  core::Config cfg;
  core::ModelTier t;
  t.model = "claude-haiku-5-5";
  cfg.models["cheap"] = t;
  t.provider = "openai_compat";
  t.model = "qwen3-coder";
  cfg.models["local"] = t;
  auto j = code::claude_agents_json(cfg);
  REQUIRE_MESSAGE(j, (j ? "" : j.error().str()));
  auto v = nlohmann::json::parse(*j);
  CHECK(v.size() == 1);
  CHECK(v["karkas-cheap"]["model"] == "claude-haiku-5-5");
  CHECK(v["karkas-cheap"]["description"].get<std::string>().starts_with("Karkas agent on model tier cheap"));
  auto prompt = v["karkas-cheap"]["prompt"].get<std::string>();
  CHECK(prompt.starts_with("You are a Karkas agent"));
  CHECK_FALSE(core::contains(prompt, "karkas:generated"));
  CHECK_FALSE(core::contains(prompt, "---"));
}
