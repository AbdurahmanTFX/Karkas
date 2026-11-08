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
