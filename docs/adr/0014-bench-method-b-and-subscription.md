# 0014 — Benchmark: method B with the whole Karkas method, and runs on a subscription

Status: accepted (deviation from spec 17.4–17.6)

## Context

Method B (Claude Code + Karkas MCP) ran `claude -p --bare`. --bare skips CLAUDE.md discovery and
project subagents, so B saw only the short MCP instructions: no guide, no subagents of the model
tiers (ADR 0013). S1 had no project.ksl, so Karkas had nothing to serve. The method stops for the
user's approval after kk_propose_change, and nobody approves in an unattended run. And every run
needed an API key, while the user works with Claude Code on a subscription.

## Decision

- Method B gets the whole method:
  - S1: `karkas init` in the fresh copy (idea: "as specified in spec.md"); S2/S3: the guide is
    written into CLAUDE.md of the copy.
  - The subagents of the tiers of the copy's karkas.toml go to `--agents` (.bench-agents.json,
    `code::claude_agents_json`); the MCP server to `--mcp-config` + `--strict-mcp-config`.
  - With --bare the guide comes through `--append-system-prompt-file CLAUDE.md`.
  - The prompt says the run is unattended: the agent approves its own proposals with
    `karkas approve C-NNNN | L0 | api`. Every approved change, a newly approved top level and a
    newly frozen api/ set count as one intervention each, as approvals do in method C.
- Method A gets `--strict-mcp-config` without a config: no MCP servers at all.
- `--auth api|subscription` (A/B):
  - api (default): `--bare`, ANTHROPIC_API_KEY required, exact costs (as before).
  - subscription: no --bare; ANTHROPIC_API_KEY and ANTHROPIC_AUTH_TOKEN are removed from Claude
    Code's environment (`runner::RunOptions::unset_env`) so the claude.ai login is used;
    `--setting-sources project,local` and `--disable-slash-commands` keep the user's own settings,
    hooks, plugins and skills out; `--no-session-persistence` for both modes.
  - Results carry `auth`; the report shows subscription runs apart ("<model> (sub)"): their
    costs are Claude Code's estimates, not money spent.
- Hidden tests stay outside the repository; the path goes to `[bench] hidden_tests` of
  ~/.config/karkas/config.toml.

## Consequences

- A against B compares Claude Code with and without Karkas on the same model and budget; in B
  the subagents may run other models (the tiers), which is the point of the method.
- Subscription runs cost no money but count against the plan's limits; a long series may stop on
  a limit (the run then fails with Claude Code's error in the notes).
- Method C still needs an API key.
