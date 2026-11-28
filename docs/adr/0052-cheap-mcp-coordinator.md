# 0052 — Mode mcp: the main session only coordinates, a subagent per run with the two turn tools

Status: accepted (extends ADR 0024 agents through MCP, 0026 seen runs, 0029 cheaper tickets)

## Context

A session in ~/.config/hypr (2026-11-28, tickets T-0001..T-0005, mode mcp, Auto) cost about $18.8 at API
prices. Of that, $16.4 was the coordinating Claude Code session on Opus and $2.2 the subagents that served
Karkas's agents. `karkas cost` showed $2.37, because it sees only what the agents were shown
(token-cost-notes.md). The coordinator's money was mostly its own context: 231 requests re-read a context
that grew to 428K tokens, $10.1 of it cache reads. The context filled because:

- the instructions told the session to serve a run itself whenever its model was the one named, so each
  step of the Opus orchestrator stayed in the main context (~58K characters);
- 34 of 48 `kk_agent_next` calls were `list` calls, made only to learn which run waits next, because
  "Run … ended" named nothing (bugs.txt 25 fixed the repeated results);
- the session read the code, built and tested on its own before and after the tickets (~121K tokens of
  command output);
- the karkas-<tier> subagents had every Claude Code tool and the prompt for block work as well. Some
  subagents also served ten runs or more in one context: Haiku reached 189K tokens, where its price is
  five times higher above 100K.

## Decision

- **The main session coordinates and serves no turn.** The mode mcp instructions
  (`prompts/mcp_instructions_agents.txt`) say: give the request to kk_task / kk_debug with the facts you
  have; do not read, build or test the project yourself; hand each run that waits to a new subagent of
  the tier it names; one ticket per subagent; after the ticket report its result and stop; one feature per
  session. The guides (`karkas_guide.md`, `karkas_guide_full.md`) say the same.
- **Every answer names the next step.** kk_task / kk_debug / kk_ticket_run wait up to 20 s for the
  ticket's first run and name it with its tier and subagent (`McpTurns::first_run`). When a run ends,
  kk_agent_act and kk_agent_next(run) wait up to 20 s and name the next run of the ticket. The answer says
  to serve it only if it names you, otherwise to stop. If the ticket has ended, it says only that
  (`McpTurns::after_run`). A subagent never gets the ticket's result: the result is shown once, to the main
  session's `list` (bugs.txt 25).
- **In mode mcp a subagent only serves turns.** `karkas gen agents`, `karkas mode` and the mode switch
  in karkas-gui write `claude_agent_mcp.md`. It has `tools: mcp__karkas__kk_agent_next,
  mcp__karkas__kk_agent_act, ToolSearch` and a prompt of ten lines: serve the run, go on only with a next
  run that names you, report in one or two lines. In mode api the subagents stay as they were, with every
  tool and the block-work prompt.
- **The whole turn says less.** The hand-off sentence of the turn preamble is gone: the coordinator no
  longer serves turns, so nobody reads it.
- `[agents] launch = "claude"` (ADR 0029) stays the cheapest choice: one headless session per run, with a
  4K base context and no coordinator in the loop. The instructions name it.

## Consequences

- The main session's context holds the request, kk_task's answer, one line per subagent report and the
  result. The turns go to subagents whose context ends with their ticket.
- A run whose tier differs from the subagent's goes back to the main session in one line. That is one
  more spawn per tier change, in exchange for a context that does not grow across tiers and tickets.
- The 20 s wait after a run's end is a guess. The engine may build or test for minutes between runs.
  When it does, the answer says that no run waits yet, and the main session waits with `list`.
