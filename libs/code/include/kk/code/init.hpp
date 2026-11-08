#pragma once
// The frame of a new target project (`karkas init`, the GUI "New project"): project.ksl,
// karkas.toml, cmake/user.cmake, .gitignore, CLAUDE.md (guide), .claude/agents/ (subagents of
// the model tiers), blocks/, tests/integration/, .karkas/.
// Works in an empty folder and in a folder that already holds code (then `karkas import`).
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kk/code/gen.hpp"
#include "kk/core/config.hpp"
#include "kk/core/error.hpp"

namespace kk::code {

struct InitResult {
  std::string name;                  // project id
  bool existing_code = false;        // the folder already had C/C++ code: next is import
  std::vector<std::string> written;  // project-relative files created (existing ones are kept)
};

// Project id from a folder name: "Flappy-Birds" → "flappy_birds"; "" when it cannot
// start with a letter.
std::string project_id_from(std::string_view folder);

// True when the folder (outside build*/, hidden folders, third_party/, _deps/) holds C/C++ code.
bool has_code(const std::filesystem::path& dir);

// Writes the Karkas guide for coding agents into <root>/CLAUDE.md (Claude Code reads it at
// start): a block between "<!-- karkas:begin -->" and "<!-- karkas:end -->", created, replaced
// or appended; the rest of the file is kept. Returns "created", "updated" or "unchanged".
kk::Result<std::string> write_agent_guide(const std::filesystem::path& root);

// Claude Code subagents of the model tiers (ADR 0013): .claude/agents/karkas-<tier>.md with
// `model:` from [models.<tier>], one per Anthropic tier, so that an external Claude Code
// session can hand a block to the model kk_model_for names. Generated files (karkas:generated)
// are rewritten and the stale ones removed; a file without the marker belongs to the user.
kk::Result<GenResult> write_claude_agents(const std::filesystem::path& root, const core::Config& cfg);

// The same subagents as a JSON object for `claude --agents` ({"karkas-cheap": {"description",
// "prompt", "model"}}), for sessions that do not read .claude/agents/ (claude --bare).
kk::Result<std::string> claude_agents_json(const core::Config& cfg);

// Creates the frame in `root` (the folder is created when missing). Fails when the folder
// already holds a project.ksl or its name gives no project id.
kk::Result<InitResult> init_project(const std::filesystem::path& root, std::string_view idea = "");

}  // namespace kk::code
