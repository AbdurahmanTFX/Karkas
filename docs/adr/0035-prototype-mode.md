# 0035 — Prototype mode: the human sketches, the architect builds, the program checks

Status: accepted (amends 13.1 new project, 14.6 editing on the picture)

## Context

The user asked (2026-11-20) for a prototype mode: switch it on, add blocks and draw rough dependency
schemas of the project as one wants to see it; these blocks are ghosts in a color of their own; on
approval the AI develops the project from the original request and the APPROXIMATE schema — it may
change the schema or add to it, but the main logic of the schema and its dependencies stays, so the
user sees the project the way they want; and if the sketch is really bad, the AI proposes other
variants — not a new schema, the drawn one changed in its critical places — and says why.

## Decision

The sketch (`kk/agents/prototype.hpp`, `.karkas/prototype.json`): a level (top or a group), the idea,
blocks (name, optional purpose and kind, place on the canvas), dependencies `from -> to` (blocks of
the sketch or blocks that exist), the architect's renames (`map`) and whether it was given to the
agents. Not KSL: it is not the schema, and nothing reads it but the GUI, the architect's task and the
check.

The check, without a model: a sketch block is kept by the block of the same id (or the one `map`
names); a dependency `a -> b` by any link, in either direction, between a block of a's subtree and one
of b's; a dependency through a group that is not expanded yet is pending, not missing (groups get
links only between their children). `unexplained_gaps`: a gap is explained when the architect's
deviations name the block, or both ends of the dependency.

The architect (`karkas design --sketch`, `karkas expand <group> --sketch`):
- The task carries the rules and the sketch: keep its blocks (same ids; a block may become a group,
  get a better purpose, kind, ports) and dependencies; develop the rest freely; rename only through
  `report.map`; leave a part out only with its reason in `report.deviations`.
- `report(done)` is refused while the sketch is not kept and a gap is not explained — the model keeps
  it or explains it in the same run (like the other report checks).
- `propose_alternative(title, reason, diff)`: a better variant of a critical place (a cycle, a block
  with two jobs, a missing piece), checked like any proposal and stored as a pending change
  C-NNNN from "architect prototype"; the cards stay as drawn. The human sees each variant as a ghost
  on the schema and approves or rejects it (ADR 0021): the variants are changes of the drawn schema,
  never a second design.
- The result names what is kept, renamed, left out (with why) and the variants (C-NNNN), which the
  chat turns into "show on schema / approve / reject" buttons. Later expansions of a group the sketch
  linked get the sketch's dependencies of that group in their task.

The GUI (ui_prototype.cpp, the canvas part in ui.cpp):
- "Prototype" in the schema toolbar; a violet bar under it: the idea, "+ Block", "Approve: to the
  architect", start anew, leave; counts and, after approval, what the schema keeps; the dependencies
  as removable chips.
- Ghost blocks are nodes of the canvas (dashed violet, moved and deleted like blocks; double click or
  their menu edits them, right click on the background adds one there); dependencies are drawn by
  dragging from a ghost's point to another ghost or to any point of a real block, dashed violet with an
  arrow — green once the schema keeps them, dotted while pending.
- Approve: the top level of an empty project → `/design --sketch`; an empty group → `/expand <g>
  --sketch`; a level that has blocks → the orchestrator gets a task (`sketch_task`, limited to the
  group, ADR 0034) and proposes the change. Sent again later, only what the schema lacks goes, as a
  task.
- Blocks the schema gets take the places they were drawn at (saved as manual positions once, moved in
  the canvas at once): the level looks the way the human drew it; ghosts that are not realized stay
  with "not in the schema".

## Consequences

- An empty level shows the canvas (not the explanation) while the prototype mode is on.
- Not verified here: a real architect run on a sketch (needs an API key and money); the chat wiring was
  checked (the command starts and reports the missing key).
- Tests: the sketch file, its own problems, the check (renames, links through subtrees, pending links of
  groups, links in either direction), explained gaps and map parsing, the task text; the architect with
  a sketch (rules and sketch in its first request, an unexplained gap refused in the same run, a variant
  stored as C-0001 and named in the result, the sketch marked sent); a later expansion gets the
  sketch's dependencies of its group. GUI checked under Xvfb: ghosts and dependencies drawn, a block
  added through the dialog, a dependency dragged, approval starting `/design --sketch`, a simulated
  design shown against the sketch (kept 4/5 blocks, 1/4 dependencies, the rest pending) with the
  realized blocks at the drawn places.
