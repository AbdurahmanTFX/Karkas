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

## Agents through MCP (mode mcp)
`[agents] mode = "mcp"` in karkas.toml (the switch in karkas-gui, `karkas mode mcp`): the work is
done by Karkas's own agents — orchestrator, navigators, workers, testers — exactly as with an API,
and you are their model. The MCP server's instructions say so when it starts.
- A feature goes to kk_task, a bug to kk_debug (a ticket, as `karkas task` / `karkas debug`).
  Do not change the project with your own tools: the agents do it in their sandboxes.
- A request the user limits to one block (or group) passes it as `block`: the descent starts there
  and the changes stay inside it. The user's message about an existing ticket goes to
  kk_ticket_run(id, note): it becomes a fact of the ticket and the ticket runs again.
- A rough structure the user describes ("blocks A, B, C; A uses B") is a prototype:
  kk_prototype(action=set, idea, blocks, dependencies). The user sees and edits it in karkas-gui
  and approves it there; send it yourself (action=send) only when asked. The architect keeps its
  blocks and dependencies and offers variants of critical places as changes C-NNNN;
  kk_prototype(action=view) shows what the schema keeps of it.
- kk_agent_next(list=true) shows the turns that wait: ticket, role, block, the tier of its
  model ([mcp.levels], [mcp.roles], [mcp.blocks]) and the Claude Code subagent of that tier.
- Serve a run yourself when you run that model; otherwise hand the run to the subagent
  (karkas-<tier>): "serve karkas run <run>". Runs of different blocks can go in parallel.
- A turn is what an API would get: the agent's system prompt, its tools, its messages. Answer
  with kk_agent_act(turn, calls=[{name, input}], text): only the agent's tools; Karkas runs them
  and shows the results in the next turn. The run ends with the tool its instructions name.
  When the results take longer than kk_agent_act waits, go on with kk_agent_next(run, after=<the
  turn you answered>): only what is new comes back, not the whole turn. Pass seen=[the runs you
  served in this session] too: a new run of a role you served (every decision of the orchestrator
  is one) comes without the system prompt, tools and unchanged context you have.
- A feature is planned in one decision of the orchestrator (dispatch with next): its blocks run one
  after another, and the ticket closes when the last is done. With Auto a worker's card change is
  approved in its own run. `karkas stats <ticket>` shows the runs and the traffic of the turns.
- Go on until kk_agent_next says the ticket ended: that answer carries its result (what each block's
  agent did, the checks); report from it. After the ticket run nothing else yourself: no approve, no
  build. Proposed schema changes wait for the user (kk_changes); after approval: kk_ticket_run, and
  kk_implement builds the code of an approved new schema.
- `[agents] launch = "claude"`: Karkas serves the turns itself, one headless Claude Code per run
  (`claude -p` on the claude.ai login, the model of the run's tier, only kk_agent_next / kk_agent_act,
  started outside the project); then nobody serves turns: kk_task (or `karkas task` in a terminal)
  and wait for the end. The sessions' streams and costs are in .karkas/mcp/launch/.
- Commands that run agents (karkas approve, expand, implement, ticket run) wait for you to serve
  their turns: never run one in the foreground of your own shell; use kk_implement /
  kk_ticket_run or the background, or the command waits on you while you wait on it.

## Models and delegation
karkas.toml decides which model works on which part of the tree ([blocks], [levels], [roles]).
In mode mcp the maps [mcp.blocks], [mcp.levels], [mcp.roles] decide (Claude tiers). [levels] pick the
model of the roles that decide at a level (orchestrator, navigators, architect, ask); the work on a leaf
(worker, tester) goes by [roles] and the size of the task and escalates on failure ([routing] levels_for
= "all" pins every role by level). `karkas stats` shows how the runs of each role and tier ended.
This is part of the method, not a hint. kk_view ends with the model line of the block;
kk_model_for <block> explains it.
- The line names a subagent (karkas-strong, karkas-medium, karkas-cheap, ...). If this session
  runs that model, do the work here. If not, larger work on that block (new functions, a whole
  leaf, its contract tests, visual work included) goes to the subagent (Agent tool,
  subagent_type karkas-<tier>); a fix of a few lines you do yourself. Hand out independent blocks
  at once and wait for their reports instead of polling.
- The brief is the subagent's only context, and a smaller model does exactly what it says.
  Start it with kk_brief <block>: Karkas fills in the card, the files, the guarantees, the api the
  block may use, the project rules, how to test and what not to touch. Your part is the [fill]
  lines — the task and why; the spec with concrete numbers (sizes, colours, timings, limits, edge
  cases), what good looks like and what to avoid; the checks of "done" — plus the facts you
  already found (files, lines, causes). Never hand over a brief with a [fill] left.
- When it reports, run kk_review <block> and read the diff against the guarantees and the
  brief: every inv line holds, only the block's files changed, the tests pass. Send it back with
  concrete corrections; fix only small things yourself.
- Work judged by eye (graphics, effects, fonts, layout) is delegated too, with a visual spec in
  numbers: sizes in px, colours as RGB, durations in ms, easing, counts, positions, font size and
  weight, what it must not look like. The subagent delivers a preview image (rendered with the
  project's code, or a faithful mock-up of the same drawing) and names its path. You open the
  image yourself and accept only what looks right; otherwise send it back with concrete
  corrections in numbers.
- You keep the design, the schema changes, the review and the integration.
- Not a Claude model: do the work here and tell the user. Missing subagents: `karkas gen agents`.

## Imported projects (own CMakeLists.txt)
- Any language (C/C++, Python, JS/TS, Go, Rust, Java, Kotlin, C#, shell, Lua, Ruby, PHP and more):
  `lang` of project.ksl names the languages of the project (`lang cpp bash lua python`); import,
  the orphan check and kk_locate take their code. In C/C++ the headers are a block's api; in every
  other language a file is its own api: its public names (`export`, `pub`, a capital in Go, no
  leading `_` ...; a shell script's subcommands, the labels of its `case "$1" in`) are the
  contract, a new or changed one is a card change. Karkas generates builds and tests for C++ only:
  another language builds and tests with `[project] build_command` and `[test] command`.
- Blocks name their existing files (`files`). A new block may name files that do not exist
  yet (warning W07): propose the card first, then write the files.
- Karkas never writes the project's CMakeLists.txt: add new source files to it yourself.
- Contract tests go into the project's own test suite. kk_run_tests builds the project and runs
  `[test] command` of karkas.toml (set it once, e.g. `command = "build/test/run_all"`;
  extra configure flags go to `[project] cmake_args`). A project with its own presets or build
  tree sets `[project] build_command` (e.g. `"cmake --build --preset linux-release"`): Karkas
  then builds with it and configures nothing.
- Karkas's agents work inside imported blocks too (kk_task, kk_debug): a worker changes the files
  of its card in place; the public part of its headers is the contract (a new public function is
  a card change), the private part, inline bodies and new constants are free; data files of no
  block come to a block with `+ files <path>`. Its check is the project's own build and tests:
  a worker's build() compiles only its files (by compile_commands.json, seconds), and the full
  build and tests run once after the last block of the ticket. `[project] verify_command` (a debug
  build without LTO) makes that check quicker; the release build is then yours.
- Compiling is not proof: with `[test] dir`, `guide` and `command` a feature ends with a behaviour
  test that a tester writes in the project's own framework and the test command runs (it gets the
  ticket's test files in KK_TEST_FILES); a failing test goes back to the orchestrator.

## Changing an existing schema
- A new block, link, port or function is a schema change: kk_propose_change with
  "~ @block <id>" and then "+ <line>" / "- <line>". Never edit block.ksl or a frozen api/
  silently.
- Run kk_dependents <block> before touching an api/: a changed contract marks every dependent
  block for review.
- Code calling a block that its card does not link to is drift (D02): fix the schema (change)
  or the code. kk_drift lists all drift.

## Stay on your task
Do only the task you were given. A bug you notice on the way that is not your task: do not fix
it. Record it with kk_note_bug (what is wrong, where, the evidence), name it in your report, and
go on. It becomes an open ticket; the user (or the session that gave you the task) decides when
and by whom it is fixed. A subagent reports such finds to its main session the same way. When the
block already has an open bug ticket, kk_note_bug answers with it instead of a second one; and
kk_run_tests lists the failures that have open tickets as known — investigate one only if it is
your task.

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
