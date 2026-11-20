# 0036 — MCP: requests limited to a block, messages about a ticket, the prototype

Status: accepted (amends 15.3 MCP tools, ADR 0024; completes ADR 0034 and 0035 for MCP clients)

## Context

ADR 0034 (scope of a request) and 0035 (prototype) were reachable from karkas-gui and the CLI only;
"Not done: the MCP tools kk_task / kk_debug take no block". The user asked (2026-11-20) to adapt them
for MCP too: a client such as Claude Code drives Karkas's agents through kk_task / kk_debug /
kk_ticket_run (and in mode mcp also serves their turns).

## Decision

- kk_task and kk_debug take `block`: the ticket is limited to that block and its subtree (the scope
  field of ADR 0034; an unknown block is refused at once, not in the background run).
- kk_ticket_run takes `note`: the user's message about the ticket (`karkas ticket run --note`); a
  closed ticket runs with a note without force.
- kk_prototype(action, level, idea, blocks, dependencies):
  - view — the sketch and what the schema keeps of it (`sketch_report`: kept / missing / pending per
    block and dependency);
  - set — a sketch from the user's description (`sketch_from_json`; bare names in dependencies are
    blocks of the sketch, other ids existing blocks; checked by `sketch_problems`; places laid out by
    `lay_out_sketch`: columns by the longest dependency chain); it replaces the sketch there; the user
    sees, moves and edits it in karkas-gui and approves it there;
  - send — gives it to the agents in the background, only when the user asked for it.
- One choice for both sides: `plan_prototype_send` (kk_agents) decides design --sketch / expand <group>
  --sketch / a task limited to the group / a task with only what the schema lacks; karkas-gui's
  "Approve" and kk_prototype(send) use it.
- A design or expansion marks the sketch "sent" itself when it ends (with the architect's renames,
  merged into the file as it is then); only a task is marked when it starts. karkas-gui reads the sketch
  again when the file changes, so it neither shows a failed run as sent nor overwrites what the
  architect wrote.
- Instructions: one sentence in both MCP instructions (block / note / kk_prototype in mode mcp, the
  prototype in the default text) and a paragraph in kk_guide (Agents through MCP).

## Consequences

- The default MCP instructions are a sentence longer (the golden session was regenerated: only that
  sentence changed).
- Tests: the send plan (design, missing idea, expand, a task limited to the group, not a group, a gone
  level, a resend with only what is missing, nothing missing), the report, sketches written as JSON
  (ids, existing blocks, places), and the MCP wiring of block, note and kk_prototype. End to end on
  copies with `karkas mcp`: view / set / a bad name refused / send (the design starts in the background;
  without a key it fails and the sketch stays a draft); kk_task with an unknown and a real block (the
  ticket's first step is the human's pick of the block, then its worker), kk_ticket_run with a note (the
  fact is in the ticket).
