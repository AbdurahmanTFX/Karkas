# 0034 — The scope of a chat message: the whole project, one block, one ticket

Status: accepted (amends 10.2 ticket format, 13.2/13.3, ADR 0019 orchestrator chat)

## Context

The user asked (2026-11-20): when a block is selected on the schema, the chat should say that the
request is made within that block and the changes go into it; a global mode gives the request to the
whole project and the global orchestrator; when a ticket is selected, the chat should say the request
is about that ticket and the ticket is what changes. Until now every message became `karkas task` /
`karkas debug` for the whole project, and continuing a ticket was `/ticket run` without a way to say
anything new to it.

## Decision

- Ticket field `scope <block>` (10.2 extension): the block and its subtree the human limited the
  ticket to. `karkas task|debug --block <id> "..."` creates such a ticket (an unknown block is refused
  before a ticket exists).
- The engine (deterministic first, 0.2): a scoped ticket's first run starts at its block — the program
  writes `L0 human at=root pick=<block> why="the human limited the request to this block"` and the
  descent goes straight to the block's navigator (group) or worker (leaf), without an orchestrator
  decision. Whatever comes back up (a feature's leaves done, not_here past the scope, later runs) goes
  to the orchestrator, whose `dispatch` and plan (`next`) may name only the scope or blocks inside it
  (`SchemaCallbacks::limit`; a refused dispatch is a tool error the model corrects in the same run),
  and its task says so; escalation is the way out when the request needs a block outside. A descent
  that climbs past the scope is put back to the orchestrator. The code index hint (`locate`) is left
  out for a scoped ticket. Changes proposed through @change are not limited (a link to an outside
  block may need its card); the human approves them as before.
- `karkas ticket run <id> --note "..."`: the human's message about a ticket becomes one more original
  fact `human: <text>` (every agent's path view shows the original facts), then the ticket runs —
  forced, since the human gave something new; a closed ticket reopens.
- GUI chat: a "Request:" row — whole project / block <name> / ticket <id> — follows the last block or
  ticket selected (on the schema, in the tree, in the tickets tile), "whole project" until the next
  selection; a colored line says what happens to the message. Block mode sends `task|debug --block`,
  ticket mode `ticket run <id> --note` (task/bug is off there); a `/command` is that command in any
  mode. The user's bubble names the scope. The inspector has "Chat" (a request for this block) and a
  ticket's menu "Write about this ticket in the chat".

## Consequences

- Tickets written before this have no scope and behave as before. Editing a ticket by hand (ADR 0033)
  can set or drop its scope; a scope that is no longer a block stops `ticket run` with a message.
- Not done: the MCP tools kk_task / kk_debug take no block yet (external agents pick blocks
  themselves).
- Tests: a leaf scope runs its worker first (no orchestrator decision until the feature closes, the
  trail line, the scope line), a group scope starts at its navigator and the orchestrator's dispatch
  outside the scope is refused, an unknown scope is refused before a ticket exists, a message becomes
  a human fact and reruns a closed ticket (a closed ticket without one still needs --force); chat
  commands of each mode. GUI checked under Xvfb (block selected → block mode, ticket selected →
  ticket mode).
