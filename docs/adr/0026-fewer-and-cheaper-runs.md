# 0026 — Fewer and cheaper runs per ticket

Status: accepted (amends 9.2 contexts, 9.6 cache prefix, 10.7 changes, 11.3 descent, ADR 0011 models
per level, ADR 0020, ADR 0024, ADR 0025)

## Context

Karkas exists to make work with AI cheaper and better than without it. Measured on flappy-karkas
(T-0013, T-0014, served through MCP) it was 3–5 times dearer than doing the same work directly:

- a ticket over three blocks took 15 agent runs, 8 of them decisions of the orchestrator, which was
  asked again after every finished leaf;
- every run started whole: 86 % of what the MCP client read were whole turns (system prompt, tools,
  context, 9–28 K characters); half of an orchestrator turn never changes between its runs;
- a worker turn carried every neighbor header in full (`config.hpp`, 4.5 K, to every block), every
  unit test of the block, and the last result of other blocks;
- the role prompts named the block, so no two blocks of a role shared a cached prefix;
- a worker that needed a card line went up to the orchestrator and came back in a second whole run
  (C-0007, C-0009), even with Auto;
- three new constants took two worker runs; a declaration the card already names took a worker step;
- [levels] pinned every role, so leaf work at level 2 ran on the strongest model;
- the MCP client's tokens were not measured at all.

## Decision

Fewer runs:
- **Plan.** The orchestrator of a feature gives the whole plan in one decision: `dispatch` and
  `propose_change` take `next` (blocks in order, each with difficulty and why). The engine sends them
  one after another, as dispatches marked `plan=k/n`, and closes the ticket when the last is done. It
  asks the orchestrator again only when a block does not end done (with "Planned and not sent yet"),
  or when the plan is done but an approved change of the ticket also changes a block nobody sent. A
  plan that waits for a human's approval is kept (`.karkas/tickets/<id>.plan.json`) and goes on in the
  run after the approval. Bugs keep one dispatch per decision.
- **Auto in the worker's run.** With `auto_approve` a worker's card diff (report
  needs_contract_change with a proposal) is approved by the tool itself; the result is the new card and
  the worker goes on in the same run with its api/ unfrozen (at most three per run).
- **Mechanical edits.** A change may carry `+ const <name> <C++ type> <value> // note` lines; on
  approval the program writes them into the block's api/ after its last constexpr, in its style (a
  frozen api/ stays frozen with the new hash). A new `fn` of a leaf card is declared by the program when
  the header lacks that name, every type is builtin, std:: or declared in that header, and the style is
  plain (free functions in a namespace, or one interface of pure virtual methods); otherwise the worker
  does it.
- **Review (opt-in).** `[agents] review_changes = true`: before an orchestrator's change is approved,
  each leaf it changes is asked (cheap) whether the card is enough for its part; a gap sets the change
  aside and the orchestrator amends it, once per run.

Cheaper runs:
- **One prefix per role.** The role prompts no longer name the block ("the block of the @view in your
  context"), so tools + system are the same for every block of a role: the API cache serves them across
  runs. The orchestrator's prompt carries a three-line KSL legend; the whole reference is the tool
  `ksl_reference()`.
- **Lazy context.** A neighbor header up to 2 000 characters comes whole; a bigger one comes as the
  declarations whose names the block's card, api/, tests, mocks and (worker) impl/ use, with the rest one
  read_file away. Unit tests come as a list of files with their TEST_CASE names. Below the root the last
  result of a feature shows only that block's lines.
- **MCP: what the client has is not sent again.** `kk_agent_next(seen=[runs])`: a new run of a role the
  client served (same system prompt and tools, recorded in `.karkas/mcp/shown/`) comes without them, and
  the sections of its first message that did not change are one line each. With `after` (ADR 0025) the
  turns of a run are deltas.
- **Cheaper models where they do.** [levels] pick the model of the roles that decide (orchestrator,
  navigator, architect, ask); a leaf's worker, tester, integration tester and composer go by [roles] and
  the size of the task, escalating on failure. `[routing] levels_for = "all"` restores the old pinning.

Measured:
- In mode mcp every turn is recorded with what the client was shown and what it answered
  (`.karkas/mcp/traffic/<run>.jsonl`); the engine turns that into the run's tokens and cost (an estimate
  of what went through Karkas, not of the client's own context), and the ticket's cost line says so.
- `karkas stats`: per role and tier, runs, how many ended well, mean steps, cost, retries — the data to
  route by; `karkas stats <ticket>`: its runs and the traffic of its turns.

Found on the way: tests whose TEST_CASE name has a ';' were cut by CTest's discovery and "passed" with no
case run (nine of them). `kk_add_tests` now fails a test that ran no case, and the names were fixed.

## Consequences

- For T-0014 the same work would take 1 orchestrator decision instead of 8 (plus one only on a
  deviation), no second whole worker run for C-0009, no worker run for the constants, worker turns about
  a third smaller, and orchestrator turns after the first about half as long through `seen`.
- A plan is trusted as given: a worker's mention of another block no longer brings the orchestrator back
  by itself; the scope check at the end of the plan is the safety net.
- Generated declarations are conservative; a header the program cannot read safely is left to the worker.
- The ticket's cost in mode mcp is no longer 0: it is the estimate, marked as such.
- Tests: libs/agents/tests/economy_test.cpp (plans, a deviation, a plan short of the change's scope, a
  plan after a human's approval, Auto in the worker's run, constants, declarations, KSL types, one prefix
  per role, lazy context, review), mcp_mode_test (seen, traffic estimate), router/config tests (levels for
  deciding roles), tickets test (last result per block), metrics test (stats).

## Addendum — measured on flappy-karkas T-0015, and two fixes it showed

T-0015 ("a pickup adds 2 points, the HUD shows +2 for half a second": config, rules.round, view.hud) ran
through MCP with this ADR. One decision of the orchestrator (a change with const lines and a plan):
the program wrote the two constants and declared `bonus_left()`; the plan ran rules.round, then
view.hud, and closed the ticket. All leaf work ran on the cheap tier (Haiku subagents); a tester on
the cheap tier also fixed the contract autopilot for the faster world (on its second try).

Client cost by the model of T-0013/T-0014: T-0013 $1.15–1.40, T-0014 $1.95–2.42, T-0015 ≈ $0.31–0.34
(main session $0.26–0.29 for 19 calls, six Haiku subagents ≈ $0.05 for 364 K tokens). The same work done
directly, without Karkas, cost $0.26–0.40 in T-0013. 8 runs instead of 15; 2 orchestrator decisions
instead of 8 (the second only because the first run's engine lacked the next fix); the second
orchestrator turn was 4.5 K characters instead of ~11 K through `seen`.

Fixes it showed:
- A tester with a task of a ticket (correct a contract test, cover new invariants) failed once at the
  cheap tier and the ticket went to the human. Such a tester now escalates its tier like a worker
  (cheap → medium → strong, the previous attempts in its task).
- Turns handed out by kk_agent_act (the deltas) were not recorded in the traffic, so the estimate
  counted only whole turns. Every shown turn is recorded now.

Open: the "+2" line of T-0015 was placed by a cheap worker at y = 94 under a 64 px score that starts at
y = 44; nothing in the pipeline looks at the picture (no visual check for drawing code). And a benchmark
of the agents on a big codebase needs agents on imported code first: every big project here is imported,
and agents do not edit imported blocks (ADR 0009).
