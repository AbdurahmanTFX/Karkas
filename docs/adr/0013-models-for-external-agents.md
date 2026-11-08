# 0013 — Models of karkas.toml for external agents (kk_model_for, Claude Code subagents)

Status: accepted (addition to spec 15 and ADR 0011)

## Context

ADR 0011 lets the user pick a model per level and per block, but only Karkas's own agents
(`karkas design / implement / task / debug`, API key) followed it. An external agent connected
through MCP (Claude Code on a subscription) runs on whatever model its session has; the MCP
server calls no model and cannot switch the client's model.

## Decision

- `agents::model_for(cfg, schema, snapshot, runs, block, role)` resolves the model the same way
  the engine does: the block or its nearest ancestor in [blocks] → [levels] → [roles]; a worker
  with role "auto" is routed by the size of the leaf (11.2, self-tuning 11.4, `route_block`,
  now shared with the engine); a tester of an est=L leaf starts at medium. Role "" = worker for
  a block with code, architect for a group or the project.
- MCP tool `kk_model_for(block?, role?)`: block, level, role, tier and why, the model and its
  provider, and what the client should do: hand the work to the subagent `karkas-<tier>`, or do
  it in the session when it already runs that model. karkas.toml is read on every call. A
  callback in ServerOptions, as the tools of ADR 0012.
- Claude Code subagents `.claude/agents/karkas-<tier>.md` (template target/claude_agent.md), one
  per tier whose provider is anthropic, with `model:` = the full model id of [models.<tier>].
  The body is the Karkas method for one block. Written by `karkas init`, the GUI "New project"
  and `karkas gen agents`; the GUI rewrites them when an API is saved or removed (only in
  projects that have .claude/agents/). Generated files carry `karkas:generated`: they are
  rewritten, and removed when their tier is gone; a file without the marker is the user's.
- Tiers of other providers (openai_compat, local models) get no subagent: Claude Code cannot run
  them. kk_model_for says so; the work stays in the session or goes to `karkas implement`.
- The guide (karkas_guide.md, section Models) and the MCP instructions tell the agent to call
  kk_model_for before it designs, tests or implements a block.

## Consequences

- Delegation is the client's decision: Karkas names the model, Claude Code runs the subagent.
  The main session's own model stays the user's choice (`/model`).
- A subagent starts without the session's context; the guide asks the main agent to pass the
  task and the facts it has.
- Subagent names are lowercase "karkas-" + tier with other characters as hyphens
  (`core::subagent_name`).

## Amendment (2026-11-09): delegation is part of the method, with a review

In both experiments (wavkit S3, Blobby features) Claude Code on Sonnet never called kk_model_for
and never handed work to a karkas-<tier> subagent: a recommendation ("if your model differs,
hand it over") was not enough. Now:
- kk_view ends with the model line of the block (`model_choice_line`, wired in `karkas mcp` as
  `ServerOptions::model_hint`), so the agent sees it wherever it looks at a block.
- The guide (section "Models and delegation"), the MCP instructions and kk_model_for say it as a
  rule: a session on another model does not write that block itself; it hands the block, the task
  and the inv lines to the subagent, then reviews the diff against the card (inv lines, files,
  tests) before it accepts it. The main session keeps design, schema changes, review and
  integration.
- The subagent prompt says its diff is reviewed against the card.

## Amendment (2026-11-09): the brief is a spec; visual work is checked by eye

In the Blobby round 2 the subagents (Haiku) did what their briefs said — and the briefs said
what to build, not how it should look. The effects and the font came out plainer than the
version written by Sonnet alone. The guide now asks for briefs written as specs (goal, exact
files and functions, concrete numbers, what good looks like, checks, what not to touch), a review
that reads the code and not only the report, and for work judged by eye a look at a rendered
preview before accepting it. The subagent prompt asks to follow the brief's numbers and to
preview visual work before reporting.
