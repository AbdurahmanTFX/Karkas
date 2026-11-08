# 0015 — `karkas edit`: schema edits from the command line, moving blocks between groups

Status: accepted (addition to spec 14.6 / stage 9)

## Context

`karkas import` puts every module of a flat folder at one level (Blobby Volley 2: ~55 blocks at
the top level, while the method asks for 3–9). Regrouping is the first job after an import, but
the edit operations existed only in the GUI, and none of them moved a block to another group.
An agent (or a person in a terminal) would have to rewrite dozens of cards and move folders by
hand.

## Decision

- New operations in kk_agents: `op_move(id, new_parent)` (the id becomes <parent>.<name>, the
  card folder moves, every reference is rewritten — the machinery of rename; an old group left
  empty is accepted, E06, and removed afterwards) and `op_set_purpose(id, text)`.
- `apply_edit` honours `EditOp::tolerate` for renames and moves as it already did for card edits.
- `karkas edit add-group | move | rename | purpose | remove [--dry-run]`: checked like every edit
  and applied at once (nothing changes when the result breaks the checks).
- They are a person's edits on the schema; in unattended runs an agent may use them when the
  task allows it (the benchmark's schema phase).

## Consequences

- Regrouping an imported project is a few commands instead of rewriting cards.
- These edits bypass @change approval, like edits on the GUI picture.
