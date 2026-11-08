# 0012 — Guide for coding agents and the write side of the MCP server

Status: accepted (addition to spec 15 and 13.1)

## Context

An external agent (Claude Code on a subscription, no API key for Karkas's own agents) connected
through MCP saw the schema but not the method: schema first, changes through the user's
approval, contracts, tests before code, one leaf at a time. It could only read, so schema
changes went around Karkas (hand-edited cards, or none at all).

## Decision

- `templates/target/karkas_guide.md`: the method in ~60 lines (find the way level by level,
  order of work, changing the schema, bugs, commands). It reaches agents three ways:
  - `CLAUDE.md` of the target project (Claude Code reads it at start): `karkas init` and the
    GUI "New project" write it; `karkas gen guide` adds or refreshes it in an existing project.
    The guide is a managed block between `<!-- karkas:begin ... -->` and `<!-- karkas:end -->`;
    the rest of the file belongs to the user and is kept.
  - MCP `initialize.instructions` (prompts/mcp_instructions.txt): the method in one paragraph
    and "read kk_guide once before you change anything".
  - MCP tool `kk_guide`.
- New MCP tools:
  - `kk_ksl_reference`: card syntax (the agents' reference) plus change examples.
  - `kk_check`: KSL checks and drift now, the text of `karkas check`.
  - `kk_propose_change(reason, diff)`: `agents::propose_change` — parse, run the checks of
    apply_diff without writing (`check_diff`), store as C-NNNN `from mcp`. Refused with the
    list of problems otherwise. Returns the card diff.
  - `kk_changes(id?)`: list, or one change with its card diff.
- No approve tool: approval stays with the user (GUI tab Changes, `karkas approve`). The guide
  says to approve only when the user said so.
- The mcp library stays free of kk_agents: write tools are callbacks in ServerOptions, wired by
  `karkas mcp`; a tool is offered only when its callback is set.

## Consequences

- A fresh agent gets the method from CLAUDE.md or the MCP instructions, and every schema change
  it makes is checked and waits for the user.
- Code is still written with the agent's own file tools; drift (kk_check) and contract tests
  (kk_run_tests) catch what goes around the schema.
