# 0018 — Briefs and reviews for delegation; edits in batches

Status: accepted (addition to ADR 0013 and 0015)

## Context

A smaller model does exactly what its brief says (Blobby round 2: Haiku's effects and font came out
plainer because the briefs said what to build, not how it should look). Writing a complete brief
by hand costs the delegating model many turns and is easy to get wrong, and an honest review
(reading the diff, checking the guarantees) costs several more — in the experiments the review
was done "by the report". The schema phase spent minutes composing a hundred `karkas edit`
commands and waiting on its own background scripts.

## Decision

- `agents::make_brief` / kk_brief / `karkas brief <block> [--task]`: the brief for the subagent of
  a leaf. Karkas writes what it knows deterministically — the subagent and model, the block and
  purpose, the files to change, the contract, the guarantees (one test each), the api of the
  blocks it may use with their headers and signatures, what not to touch, the project rules, how
  to build and test (the project's own `[test] command` when it has its own build), the visual
  check, the report format. The parts only the delegating model can write are `[fill]` lines:
  the task and why; the spec with concrete numbers and the look; the checks of "done". The guide
  forbids handing over a brief with a `[fill]` left and asks to add the facts already found.
- `agents::make_review` / kk_review / `karkas review <block>`: the guarantees as a checklist, the
  changed files since the last commit (files outside the block flagged; the project may be a
  folder of a larger repository), the diff of the block's files (first 200 lines), the KSL and
  drift issues of the block and its test status, and the rule for accepting.
- `agents::apply_edit_script` / `karkas edit batch <file|->`: a script of edits (add-group, move,
  rename, purpose, remove), applied in order, each checked, stopping at the first failing line.

## Consequences

- The delegating model's effort goes into the part that needs judgement (numbers, look, checks)
  instead of collecting context; the subagent gets a complete, uniform brief.
- A review is one call instead of git status + git diff + reading cards + kk_check.
