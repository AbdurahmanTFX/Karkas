# 0029 — Cheaper, faster and surer tickets: no coordinator, behaviour tests, a lighter index

Status: accepted (extends ADR 0026–0028; measured on Endless Sky, see Measured)

## Context

After ADR 0028 the Endless Sky benchmark (round 2, a "ships destroyed" statistic) showed where a ticket of
method C — Karkas's own agents in mode mcp — still spends:

- **Money: the coordinating Claude Code session, 90–96%.** C2-opus $0.54, of it the Opus session $0.52:
  15.8K tokens of Claude Code's own context written to the cache, 17.5K of the orchestrator's turns,
  15 requests re-reading the growing context. The agents' own work was about $0.06 by Karkas's estimate,
  the workers on Haiku $0.02.
- **Time: the release build with LTO (4.3–6 min of 8–10), 50–66 s of idle waiting after the ticket had
  ended (bugs.txt 10) and ~1 min of wrap-up** — coordinators even ran `karkas approve api` and tried a
  build themselves (bugs.txt 11).
- **Quality: "done" meant "compiled".** Method B's counter compiled, saved and showed, and never counted
  (it read `event.Actor()`, empty for destroyed ships); nothing in Karkas would have caught it: imported
  blocks had no tester (kImportedNoTester). The Sonnet orchestrator of C2 wrote the new function into a note
  only; the card said `-> int` over `int64_t` code and no check saw it.
- **The index** still re-checked the whole project (2.5–6 s) after every agent run, because the run
  journals were part of its fingerprint; `karkas check` re-analyzed every file (4.9 s) even when nothing
  had changed. The views of a big imported card carried hundreds of `call` lines (PlayerInfo: 407 lines,
  16.7K characters) that the neighbors section of the same view groups anyway.
- Karkas's own test suite took ~5 min, nearly all of it four tests that build reference projects.

## Decision

1. **No coordinating session** (`[agents] launch = "claude"`, `RunLauncher`). In mode mcp the engine starts
   one headless Claude Code per run: `claude -p` on the claude.ai login (ANTHROPIC_API_KEY and
   ANTHROPIC_AUTH_TOKEN removed from its environment), `--model` of the run's tier, `--system-prompt` of three
   sentences, `--tools ""` and only `mcp__karkas__kk_agent_next` / `kk_agent_act` (`karkas mcp --only-agent`),
   `--strict-mcp-config`, started in a folder outside the project (no CLAUDE.md, no memory of other sessions).
   A run is launched when its turn has waited 3 s untaken, so a parent run's session keeps serving its nested
   runs; an atomic marker per run keeps a second launcher and MCP clients away; a session that dies early is
   started once more. Streams go to `.karkas/mcp/launch/<run>`, Claude Code's cost lines to `costs.jsonl`.
   kk_task tells a client that nothing has to be served; `karkas bench --method C --auth subscription` uses it.
2. **The end of a ticket** (bugs 10, 11): `kk_agent_next(list)` returns as soon as the tickets the server
   started have ended, with their result (facts per block, verify, api, notes), and a wait for an ended run
   returns at once. With auto_approve the engine freezes the api it approved; without it the note is for a
   human. The MCP instructions say: after the ticket, only the report. `self_executable()` takes the path
   without " (deleted)" after a rebuild (bug 9).
3. **Behaviour tests of imported features** (`[test] dir`, `[test] guide`, `[test] command`). After the last
   block of a feature and its full build, a tester (prompt tester_imported) writes one test in the project's
   own framework into `dir` as the guide says (its context: the block's view and headers, the folder's tests,
   the shortest of them, the guide) and runs it with test(); the test command gets the ticket's test files in
   KK_TEST_FILES (from `.karkas/cache/ticket_tests.txt`, which lives as long as the ticket). A passing test
   is a line of the result; a failing test on a building project goes back to the orchestrator once, with
   its output.
4. **The orchestrator**: a change whose note names `fn x(` without declaring it is set aside before anyone
   works on it (no model needed); after a decision that came back (a set-aside change, a worker's
   needs_contract_change, a not_here to the root) the orchestrator decides one tier higher; a decision below
   the strongest tier gets review_changes without the setting.
5. **Smaller, surer contexts**: a view shows a card with more than 24 call lines as one line (the neighbors
   section groups them); a worker of an imported block gets the linked declarations that the functions its
   task names use (handle_event, save → HandleEvent, Save), not those of its whole 6 000-line file; D04 warns
   when a card's return type is not the code's (std:: does not count).
6. **The index**: the fingerprint has a code part (cards, code, approvals, tests, the analyzer's build) and
   a service part (tickets, changes, probes, run journals); a change of the service part only refreshes
   those tables. `karkas check` and kk_check read the issues of the updated index (drift gained the column);
   cached facts are keyed by the analyzer's build. The busy timeout of SQLite is 30 s (the GUI and the MCP
   server update one database). Logging calls (debug, info, ...) are no D09 calls.
7. **A quicker check** (`[project] verify_command`, e.g. a debug build without LTO) replaces the release
   build_command in the engine's builds when set.
8. **Karkas's own tests**: `kk_add_tests(... SLOW ...)` labels tests that build reference projects `slow`;
   `ctest --preset dev-fast` leaves them out; their builds get ccache when it is there.

## Consequences

- With launch the price of a ticket is the price of its runs; a long Opus coordinator is no longer the bill.
  Each run pays Claude Code's base context once (a few thousand tokens with the short system prompt).
- A behaviour test needs a project that can run one headlessly and a guide; Endless Sky's
  (tests/karkas/run.sh, HOWTO.md of the benchmark copy) runs a test plugin in the game's test mode and checks
  `# expect-save` lines in the pilot file. A debug build of the game loads its data ~8x slower than the
  release one (119 s against ~15 s per test), so verify_command and behaviour tests pull in different
  directions there.
- The analyzer's version is its build time: a rebuild of analyzer.cpp re-analyzes once (Endless Sky 10.6 s).
- kDbSchemaVersion 2: an index of an older karkas is rebuilt; a karkas-gui still running the old binary
  rebuilds it back — restart the GUI after updating.

## Measured

Endless Sky round 2 (the "ships destroyed" statistic), method C3: `karkas task` with launch = "claude", the
orchestrator on Opus, workers on Haiku, the behaviour tester on Sonnet ([mcp.roles] tester = "medium"), the
release build as the check. Each run found something in this ADR's own new code, fixed before the next:

| run | result | cost | wall | what it showed |
|---|---|---|---|---|
| C3 1 | escalated | $0.90 | 13.9 min | the behaviour test caught the bug of method B (a Haiku worker counted with `Actor()`); the orchestrator found the cause, but the no-circles rule forbade sending the block again; the asks of the orchestrator ran in its Opus session |
| C3 2 | escalated | $0.64 | 14.3 min | the block went again (with the test's output) to a cheap worker, which missed the cause and answered not_here; the descent wandered; the orchestrator escalated with the exact fix |
| C3 3 | **done, works** | $1.15 | 19.6 min | the fix went to Sonnet with the orchestrator's diagnosis and passed the test; a stale "the test still fails" then cost an Opus worker and three asks (~$0.57) |
| C3 4 | escalated, **works** (in the game 1 -> 2) | $1.27 | ~17 min active (3.2 h with the wait) | the Haiku worker wrote the right condition at once; the subscription's 5-hour limit stopped every session for 3 hours and the panel's worker went up to Opus (bugs.txt 15); the tester's test waited for a kill its scenario cannot give (the turret disables the pirate and stops firing, seen in gdb); the Opus fix worker said so (needs_tester) and the engine sent it to the contract tester an imported block does not have (bugs.txt 16) |
| C3 5 | stopped (weekly usage limit), **works** (in the game 1 -> 2, checked 2026-11-19) | $1.55 by Claude Code (Opus $1.14, of it $0.93 the fix worker by hand; Sonnet $0.39; Haiku $0.02), the last orchestrator session unrecorded | 47 min to the stop | the Haiku worker wrote `Actor()->IsYours()` again (4 of 5 Haiku runs); the behaviour test caught it and the orchestrator sent the exact fix to Sonnet, whose safeguards refused the turn twice (bugs.txt 18), so the fix worker ran on Opus by hand; the code was then right, but the tester's target had "required crew" without bunks, appeared disabled, the turret never fired and a random "Author" ship made the kill (bugs.txt 17); the worker found the Author kill (not_here), the orchestrator was closing when the weekly limit stopped it. With `"automaton" 1` on the target the same test passes in 10 s |

The fixes, in order: a block done in the run may be sent again after a failing behaviour test, twice, the
first a tier up (medium), the second strong, with the test's output and the orchestrator's cause; the answer of
such a worker goes straight back to the orchestrator; the tester's own test() runs the files it writes (the
test folder's files since its start); a nested run gets a session of its own tier; a passing fix ends the sends
and the orchestrator hears it. After run 4: needs_tester while the ticket has a behaviour test goes to the
behaviour tester with the worker's finding; on the usage limit no session starts for 5 minutes at a time, the
turns wait without timing out and the try does not count; the ticket's test list survives a resumed run; the
Endless Sky guide says that turrets never finish a disabled ship. After run 5: a refusal by the model's
safeguards sends the run at once to the next tier's model, the try not counted (launcher.cpp, test in
mcp_mode_test); the guide says that a target needs `"automaton" 1` (or bunks and a crew), or it appears
disabled. Still open: a not_here that blames the behaviour test goes to the orchestrator, not to the tester.

Method B on this Karkas (Claude Code on Opus with Karkas as the map, its project without a [test] command):
$0.755, 8.8 min, **2.3 min without compiling** (before ADR 0029: 14.8 and 8.6 min; the smaller views and the
indexed check), and again `event.Actor()->IsYours()`: 0 / 0 in the game; it said that nothing ran the feature.

- The launched sessions start in ~2 s with ~4K tokens of base context (the short system prompt and two tools),
  against 15.8K of a default Claude Code session.
- Of a run's money the Opus orchestrator sessions are now the largest part ($0.08-0.13 each), then the Sonnet
  tester (~$0.17-0.20); the Haiku workers $0.005-0.015 each.
- Four of five Haiku workers in C3 (and method B's Opus session, twice) wrote `event.Actor()->IsYours()`: the trap
  is natural; only a behaviour test finds it, and only a stronger model with the cause in its task fixes it.
- `verify_command` with Endless Sky's debug build: 475 s after a header change (59 steps, a 427 MB binary)
  against 261-363 s of the release build with LTO, and the debug game runs a test ~8x slower: not used there.
- Karkas's own tests: 330 (all pass 2026-11-19); `ctest --preset dev-fast` 325 in 25 s; the slow four 227 s (with a warm ccache
  the reference builds 98-106 s -> 16-19 s; the engine's real-build test 285 -> 236 s); the perf test runs
  alone (RUN_SERIAL).
