# 0024 — Agents through MCP: the same agents, an MCP client as their model

Status: accepted (amends 15 MCP, ADR 0011 models per level, ADR 0013 models for external agents)

## Context

Karkas had two ways to work with AI. Through an API its own engine runs the agents of a ticket —
orchestrator, navigators, a worker per leaf, a tester — with their prompts, sandboxed tools,
checks of every answer, escalation and journals. Through MCP an external client (Claude Code)
worked by the guide: it read the schema with kk_* tools and changed files with its own tools, so
nothing of the protocol held (no descent, no sandbox, no outcomes, no trail). The user wants the
AI through MCP to behave exactly as the AI through the API, a switch between the two, and the
model levels fitted for each.

## Decision

- `[agents] mode = "api" | "mcp"` (`karkas mode`, the switch in karkas-gui: chat header, status
  bar, settings). The engine is the same in both modes; only where the model's answers come from
  differs.
- Mode "mcp": the engine's provider is `llm::McpBridgeProvider`. Every turn — the request an API
  would get: system prompt, tools, messages — is written to `.karkas/mcp/turns/<run>-<seq>.*`
  (meta, request; `taken`, the answer) and waits for the client (`[agents] mcp_wait_s`, 1800 s;
  a stop of the run ends the wait). The answer `{text, calls}` becomes the tool calls the engine
  runs in the agent's sandbox, as if an API had answered. Journals record provider `mcp`, cost 0.
- MCP tools (`karkas mcp`): kk_task / kk_debug create a ticket and start `karkas ticket run` in
  the background (log in .karkas/mcp/<ticket>.log); kk_ticket_run continues one; kk_agent_next
  lists the waiting turns or takes one whole (system, tools with signatures, messages);
  kk_agent_act answers it — only the agent's own tools, an unknown name is refused at once —
  and returns the next turn of the run (only what is new) or how the run ended. These two run on
  their own threads, so subagents serve runs in parallel. A run that waits on a nested run of
  its ticket (an `ask`) gets that run handed to the same client, or both would wait for ever.
  The initialize instructions of mode "mcp" tell the client to give work to kk_task/kk_debug,
  never to change the project with its own tools, and to serve the turns: itself when it runs the
  model named in the turn, otherwise through the subagent karkas-<tier>.
- Each mode has its own model maps: [roles] [levels] [blocks] [routing] escalate for "api";
  [mcp.roles] [mcp.levels] [mcp.blocks] [mcp.routing] escalate for "mcp" (Claude tiers, whose
  Claude Code subagents play them). The maps of the active mode are the ones the engine,
  kk_model_for and the GUI use and write (`Config::map_section`). `karkas mode --fit` and "Fit to
  the API models" in the GUI set the MCP maps from the API ones: an Anthropic tier stays, any other
  takes the Claude tier at the same place of the escalation chain (weakest → cheap, strongest →
  strong). In mode "mcp" the GUI offers only Claude tiers.
- The subagent template serves a run ("serve karkas run <run>"); the guide has a section "Agents
  through MCP"; the short guide of CLAUDE.md names the mode.

## Consequences

- The protocol of a ticket, its prompts, sandbox, checks, escalation and journals are the same
  in both modes by construction: one engine. The GUI's live feed shows MCP runs like API runs.
- A client that does not serve the turns stops the agents: after mcp_wait_s the run ends with
  "no MCP client answered" and the ticket escalates with that reason. The GUI says when turns
  wait and nobody took them.
- The MCP client spends its own tokens (subscription); Karkas bills nothing in mode "mcp".
- Tests: libs/agents/tests/mcp_mode_test.cpp (a bug ticket through the queue with the same
  trail as through an API; a turn nobody answers; a nested `ask` run), llm bridge tests, config
  tests; `karkas mcp` checked over stdio with a scripted client (kk_task → orchestrator turn →
  escalate → ticket ended).
