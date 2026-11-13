# 0025 — Agents see why they fail, a client never waits on itself, new promises get tests

Status: accepted (amends 5.5 CMake, 9.5 sandbox, 12.1 testers, ADR 0020, ADR 0024)

## Context

Two runs were read end to end: flappy-karkas T-0013 (breathing columns, the agents served by
Claude Code in mode mcp) and the logs of test-free-AI (OpenCode as the MCP client, DeepSeek
through the API; $0.48, 101 runs). Most of the waste came from Karkas, not from the models:

- `has_cpp()` took any file without an extension (`.gitkeep`, which every new leaf has in
  tests/unit and tests/contract) for a C++ source. The leaf got `add_executable(<leaf>_tests)`
  with an empty glob and the configure of the whole project failed ("No SOURCES given to
  target"). After C-0004 of test-free-AI every tester of the project ran into it: 5 runs, 101
  steps, none could build.
- The build summary kept the line "CMake Error at CMakeLists.txt:149 (add_executable):" and
  dropped the indented message under it, so no agent learned what the error was.
- A refused read said "block X is not linked to Y" and nothing else; a missing file "cannot
  read"; a missing folder "not a folder". The integration testers tried `game:`, `core:`,
  `common:`, `shared:` for the header of shared.types: 42 of 50 steps were errors.
- An entry block (the composition root) had no test program. The bug worker of T-0003 was told
  to add a unit test first, wrote one, heard "tests none found" and spent three tiers (75 steps)
  probing gtest, CMake files and `#error` markers. Its prompt never named doctest.
- A build broken in other blocks' files did not say so; agents tried to fix or work around it.
- OpenCode ran `karkas approve` in its own shell. The architects' turns waited for that very
  client, which waited for the command: two runs cancelled after the shell's 60 s timeout. The
  implementation step had no MCP tool, and the approval text said "next: karkas implement all".
- Mode mcp: after an answer whose results took longer than kk_agent_act waits, the client
  called kk_agent_next(run) and got the whole turn again (system prompt, tools, every message).
- A run stopped by the user wrote its partial result without the state fingerprint; the next
  orchestrator was told the result came from "an older run".
- A D04 warning (other parameter counts in the card and api/) was worded "api/ behind its
  card", though api/ was the side ahead (a defaulted parameter).
- A feature that gave a leaf new `inv` lines never ran its tester: rules.columns gained four
  invariants and kept 12/12 contract tests.
- The path view folded "proposed C-0006" and "proposed C-0007" into one line with repeats=2.

## Decision

- CMake (5.5): `has_files` matches an exact extension (a file without one is no source); the
  globs list `*.cpp *.cc *.cxx`, the extensions has_cpp counts; every test program is created
  under `if(<glob>)`, so a folder emptied after generation does not stop the configure. The
  entry block gets `<entry>_tests` when its tests/ has sources: its impl/ without `main.cpp`
  (main() lives there, skel_main.cpp), the leaves and doctest's main; tests app/unit,
  app/contract.
- The build summary appends the indented lines under a "CMake Error" line (up to four).
- Sandbox messages name what is readable: "readable are your own folder and the api/ of: …";
  a group: "X is a group, which has no api/; its blocks readable here: …"; an unknown name:
  "there is no block X". read_file / list_dir of a missing path list what the nearest existing
  folder has; the own folder of an integration tester that does not exist yet is "(empty:
  nothing is written here yet)"; a file given to list_dir says it is a file.
- build / test of an agent: when no error is in a file the agent may write, the result says so
  and tells it not to fix or work around them but to do its part and name the files in cause;
  CMakeLists.txt is called Karkas's generated file. "tests none found" says how a test of the
  block is built (doctest, every tests/**/*.cpp, no main, no CMake file). The worker prompt
  names doctest.
- Mode mcp: the MCP tool kk_implement starts `karkas implement <target>` in the background
  (log .karkas/mcp/implement-<target>.log; kk_agent_next reports how it ended). The commands
  that run agents print in mode mcp that a client running them itself must do it in the
  background; the instructions of mode mcp and the guide say the same; `karkas approve` says
  "next: kk_implement". kk_agent_next takes `after` = the turn of that run the client answered
  last: a turn that follows it shows only what is new. kk_agent_act's "still working" and the
  nested-run handover name `after`; a client new to a run (a subagent) calls without it and gets
  the whole turn.
- A stopped run's partial result carries `state` like any result.
- The status line and the worker's api note tell D04 E ("the card names what api/ lacks",
  behind) from D04 W ("api/ and its card differ: …"; api/ follows the card, or a change brings
  the card in line when api/ is right).
- A feature leaf that is done and green, whose card gained `inv` lines by the approved changes
  of this ticket, goes to its tester with those lines (one contract TEST_CASE each, through
  api/). Green again: the result has `tests <leaf>: contract tests for N new invariant(s)`. The
  new tests red: the worker runs once more with the failures (it must not touch
  tests/contract; a wrong test is needs_tester); red after that: escalated with the result
  lines. The tester failing but the tests green: done, with a line that the new invariants have
  no contract tests yet.
- Path view: an own decision with outcome=change folds only with the same `why` (change id).

## Consequences

- The generated CMakeLists.txt changes for every project (globs, `if()` guards, no empty
  `<leaf>/unit` tests for a folder with only .gitkeep); the fixture wavkit holds the new one.
- A feature with new invariants costs one tester run per leaf (cheap tier by default) and
  possibly one more worker run; it closes with its promises checked.
- Not changed, for a decision: the orchestrator may send a worker to a leaf still in design (no
  api/ yet); `implement` runs testers before the workers even when an api/ changed and the
  impl/ no longer compiles (TDD order on a stale code base).
- Tests: gen (`.gitkeep`, globs, entry tests built and run, a test file removed after
  generation), runner (CMake message lines), sandbox and leaf tools (messages, build hints,
  tests none found), engine (stopped result with state; tester for new invariants), mcp mode
  (`after` gives the delta, a fresh client the whole turn), index views (D04 W wording),
  tickets (two proposals). `kk_implement` checked over stdio.

## Addendum — flappy-karkas T-0014 (the pace rises with the score), served through MCP

Five more defects showed up in one feature over config, rules.round and view.hud:

- The entry's test program of this ADR linked flappy's two standalone tests (each with its own
  `main()`) into one doctest program: "multiple definition of main" stopped the whole build. A test
  source with its own `main()` or `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` is now a program of its own
  (`<block>/<kind>:<name>`, it passes with exit code 0) — for leaves too; the others share the
  doctest program. flappy's app tests run for the first time (`app unit 2/2`).
- The api note "unfrozen but already in line with its card" said "only a declaration the card
  names"; a card never names constants, so the worker of config refused three new constants that a
  frozen api/ would take (one escalation, one rerun). The note now allows new constants and values
  like a frozen api/.
- `needs_tester` was written for bugs: after the tester's fix the ticket was finished, so the
  feature closed with its HUD part undone, and the result lost the worker's files and api line. In a
  feature the leaf now counts as done and the orchestrator goes on; the result keeps the worker's
  files and api/; a leaf's new invariants go into the same tester run.
- A closed ticket could not run again even with `--force`, and kk_ticket_run answered "runs again"
  for a start that ended at once. `--force` reopens a closed ticket; kk_ticket_run says "closed;
  force=true reopens it" before it starts anything.
- read_file with offset/limit trimmed its lines (core::split trims); a model copies them into
  edit_file's `old`, which then misses. The lines keep their indentation.

Measured (same cost model as T-0013): 73 MCP calls, 15 agent runs, 32 turns, 1 tool error; the
client read 268 K chars, 86 % of them whole turns of new runs (system prompt, tools, context).
Within a run the turns are deltas (0.5–5 K chars, `after`). Open for a decision: a delta across
runs of the same role and block, a `remove_file` tool for the agents' own folders, doctest values in
the failures an agent sees, and a worker that goes on in the same run after Auto approves its card
diff.
