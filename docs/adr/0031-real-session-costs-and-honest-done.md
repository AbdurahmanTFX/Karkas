# 0031 — The sessions' own cost, a done that changed something, a planned block answers its planner

Status: accepted (extends ADR 0026, 0029, 0030; measured on Endless Sky round 3)

## Context

Round 3 run 3 (pilot statistics, the first Karkas ticket of the benchmark that closed done and works in the game):

- **Its cost line said $0.19; its ten Claude Code sessions reported $1.08.** In mode mcp the engine estimated a
  turn from what it showed the client and what came back, at the full input price, with no cache (ADR 0026). A
  launched session (ADR 0029) spends otherwise: everything new in its context is written to the prompt cache with
  a 1-hour TTL (2x the input price) — the turn, the model's output, and first its own ~2K tokens of system prompt
  and tools; every request reads the cached context again; the output is ~5x the answer's text (thinking, the
  call's JSON); a refused session costs too. Per session the estimate was 0.2-0.3 of the real cost. `karkas cost`
  also priced every cache write at the 5-minute 1.25x, and karkas.toml had no cache read price for Sonnet 5.5
  (0.1x of $2 = $0.20; the price is $0.10).
- **A worker reported done and changed nothing** (bugs.txt 14, again): the worker of `flight.combat.ship_event`
  (Haiku) answered in one step that the counting belongs elsewhere, as done; it counted as the block's part.
- **A block of the plan that answered needs_info went up the tree** (bugs.txt 13, again): `flight.engine` (plan
  2/4) said its part was only partly wired; the descent went to the navigator of `flight`, to `flight.combat`, to
  `ship_event` — the orchestrator, who had planned the feature with the whole ticket in view, never heard it.
- **The launcher judged a session by an earlier try's result line** when it wrote none (the tries of a run append
  to one log): a usage limit paused all launches again, a refusal raised the tier again.

## Decision

1. **A run served by a launched session costs what the session reported.** The launcher's costs.jsonl rows (one
   per session; several when a run was started again) give the run's tokens (input, cache writes and their 1-hour
   part, cache reads, output) and money at list prices: the runs index takes them (a new column for 1-hour writes
   and a flag; the index is rebuilt from the journals, schema version 3; costs.jsonl is part of the service
   fingerprint, so a session that ends after its run was indexed updates it), `karkas cost` and the GUI read them,
   and the ticket's cost line sums them after waiting up to 30 s for the sessions to end
   (`RunLauncher::settle`): "usd=1.0830 (10 of 10 run(s) at the cost their Claude Code session reported, at list
   prices)".
2. **The estimate for other clients models the cache** (`McpBridgeProvider::traffic_usage`): per turn, a 1-hour
   cache write of what it showed plus its output (and 2K tokens of base context on the run's first turn), a cache
   read of the context the turns before wrote, an output of 5x the answer's text (3.5 characters a token). Fitted
   on round 3 run 3 (mean error 9%, total 1.13x); checked on round 2 runs 4 and 5 and round 3 run 1: 0.87, 0.92 and
   0.81 of the real cost (it was 0.2-0.3).
3. **`karkas cost` prices 1-hour cache writes at 2x** the input price; the template's Sonnet 5.5 has `cache_read =
   0.10`.
4. **A done with no file of its block changed is no part done** (`[agents] done_needs_change`, on by default): the
   engine hashes the block's files (the card's files of an imported block, its folder otherwise) before the worker's
   run; when a done or fixed leaves them as they were and the card's api is unchanged, the orchestrator hears
   "reported done without changing a file: <its text>" and decides.
5. **A block of the plan answers its planner**: a worker of a planned ticket that answers not_here or needs_info
   goes back to the orchestrator with its answer and the plan's remaining blocks, not up the tree.
6. **A session reads only its own part of the run's log** (its start offset, taken before it is spawned).
7. The behaviour tester's prompt says to start from a state without the result (round 3 run 3's test injected a
   save that already held the statistics and read them back) and to name the parts it cannot reach.
8. The escalation note of a worker names the block, its status and "before any model call" instead of "the
   strongest tier failed" (bugs.txt 8).

## Consequences

- The cost of a Karkas ticket in mode mcp with launched sessions is the bill at list prices, not an estimate.
- One more orchestrator turn when a planned block answers not_here or does nothing: the descent through
  navigators it replaces cost more (two Sonnet navigators and a worker in run 3) and found nothing.
- Scripted tests turn `done_needs_change` off (their workers do not write files); its own test turns it on.
