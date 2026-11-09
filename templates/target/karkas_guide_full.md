# Karkas project: how to work here

Karkas keeps this project as a tree of blocks with explicit links. The cards (block.ksl) are
the source of truth and the code follows them. Work through the schema, level by level: do
not read or grep the whole code base.

## Pick the path by the size of the task
- Bug or small change (one place, a few lines): kk_locate "<words of the symptom>" names the
  blocks, files and functions to look at. Read those, fix, run kk_check and kk_run_tests. No
  schema change and no approval, unless you add a block, link, port or function.
- Feature or a change of the structure: the order of work below, schema first.

## Find your way
- MCP server "karkas": kk_overview, then kk_view <block>, kk_neighbors, kk_dependents,
  kk_path, kk_status. Without MCP: `karkas view [block]`.
- Block `a.b` is the folder blocks/a/b/: block.ksl (card), api/ (contract headers), impl/,
  tests/contract/. Imported blocks name their existing files with `files` instead.
- Open the code of the blocks you work on and the api/ of their linked neighbors, nothing else.
- Card syntax: kk_ksl_reference. The `rule` lines of project.ksl bind every block.

## Order of work (a new project or a new feature)
1. Schema first, no code. Top level: 3-9 blocks; each group: 2-9 children; down to leaves (one
   job, one thread, up to ~400 lines and 6 ports, testable through its ports). Ids are short
   lowercase English words that say what the block is for (menu, game, render); purpose is one
   English sentence. Links (call, use, in ... from=) are written in the consumer's card.
2. Write the cards as one change: kk_propose_change with "+ @block <id>" records. It is checked
   before it is stored. Then STOP: the user reviews and approves it (GUI tab Changes, or
   `karkas approve C-NNNN`). Approve yourself only when the user told you to.
3. Contracts: api/ headers of the leaves (the functions and types their cards list) and `inv`
   lines (one checkable guarantee each). The user freezes them: `karkas approve api`, which
   generates CMake, skeletons, mocks and the probe.
4. Tests before code: tests/contract/ of a leaf, one doctest case per `inv`; they fail first.
5. Implement one leaf at a time in impl/. A leaf includes only its own api/ and the api/ of the
   blocks its card links to. Make its contract tests pass (kk_run_tests <block>).
6. After every step: kk_check shows 0 errors and the tests pass. Name the warnings you leave.

## Models and delegation
karkas.toml decides which model works on which part of the tree ([blocks], [levels], [roles]).
This is part of the method, not a hint. kk_view ends with the model line of the block;
kk_model_for <block> explains it.
- The line names a subagent (karkas-strong, karkas-medium, karkas-cheap, ...). If this session
  runs that model, do the work here. If not, larger work on that block (new functions, a whole
  leaf, its contract tests) goes to the subagent (Agent tool, subagent_type karkas-<tier>) with
  the block id, the task, its inv lines and the facts you have; a fix of a few lines or work
  judged by eye you do yourself. Hand out independent blocks at once and wait for their reports
  instead of polling.
- The brief is the subagent's only context, and a smaller model does exactly what it says.
  Start it with kk_brief <block>: Karkas fills in the card, the files, the guarantees, the api the
  block may use, the project rules, how to test and what not to touch. Your part is the [fill]
  lines — the task and why; the spec with concrete numbers (sizes, colours, timings, limits, edge
  cases), what good looks like and what to avoid; the checks of "done" — plus the facts you
  already found (files, lines, causes). Never hand over a brief with a [fill] left.
- When it reports, run kk_review <block> and read the diff against the guarantees and the
  brief: every inv line holds, only the block's files changed, the tests pass. Send it back with
  concrete corrections; fix only small things yourself.
- Work judged by eye (graphics, effects, fonts, layout): put the visual spec in the brief, then
  look at the result before you accept it — render a preview image or a screenshot and read it.
  Not good enough: send it back with concrete corrections, or do it yourself.
- You keep the design, the schema changes, the review and the integration.
- Not a Claude model: do the work here and tell the user. Missing subagents: `karkas gen agents`.

## Imported projects (own CMakeLists.txt)
- Blocks name their existing files (`files`). A new block may name files that do not exist
  yet (warning W07): propose the card first, then write the files.
- Karkas never writes the project's CMakeLists.txt: add new source files to it yourself.
- Contract tests go into the project's own test suite. kk_run_tests builds the project and runs
  `[test] command` of karkas.toml (set it once, e.g. `command = "build/test/run_all"`;
  extra configure flags go to `[project] cmake_args`).

## Changing an existing schema
- A new block, link, port or function is a schema change: kk_propose_change with
  "~ @block <id>" and then "+ <line>" / "- <line>". Never edit block.ksl or a frozen api/
  silently.
- Run kk_dependents <block> before touching an api/: a changed contract marks every dependent
  block for review.
- Code calling a block that its card does not link to is drift (D02): fix the schema (change)
  or the code. kk_drift lists all drift.

## Bugs
Descend from the symptom: kk_overview, the suspect group (kk_view), its children, down to a
leaf. Rule blocks out with facts (kk_status, kk_probes, tests); if there is a ticket, append
the trail (kk_ticket_append). Fix inside one leaf; a contract change goes through a change.

## Commands
karkas view [block] | check | test [block] | change list|show <id> | approve L0|api|C-NNNN
karkas gen cmake|skel|mocks|guide|agents | import | gui
(karkas design / expand / implement / task / debug run Karkas's own agents; they need an
API key. Without one you do the same work by hand, following this guide.)

Answer the user in the language they write in.
