# Karkas project: how to work here

Karkas keeps this project as a tree of blocks with explicit links; the cards (block.ksl) are the
source of truth and the code follows them. MCP server "karkas" (without it: `karkas view`,
`karkas query locate <words>`, `karkas check`).

## Pick the path by the size of the task
- Bug or small change (one place, a few lines): kk_locate "<words of the symptom>" names the
  blocks, files and functions to look at. Read those, fix, run kk_check and kk_run_tests. This
  needs no schema change and no approval, unless you add a block, link, port or function.
- Feature or a change of the structure: Schema first. Read kk_guide once (the full method), then
  kk_overview and kk_view <block>, and propose the cards with kk_propose_change before the code.

## Always
- Open only the code of the blocks you work on and the api of their linked neighbours.
- A new block, link, port or function is a schema change: kk_propose_change, then the user
  approves it (`karkas approve C-NNNN`; approve yourself only when the user said so). Never edit
  block.ksl or a frozen api silently.
- Do only the task you were given. Another bug you notice on the way is not yours to fix: record
  it with kk_note_bug (what, where, evidence), name it in your report and go on.
- Done means: kk_check shows 0 errors and kk_run_tests passes.
- kk_view ends with the model line of the block. On another model, hand larger work on that block
  (new functions, a whole leaf) to the named subagent: start the brief with kk_brief, fill every
  [fill] line with concrete numbers and checks, and accept the work only after kk_review. Small
  fixes and visual work you do yourself (kk_guide: Models and delegation).
- Imported project (own CMakeLists.txt): add new files to its build yourself; kk_run_tests runs
  `[test] command` of karkas.toml.

Answer the user in the language they write in.
