# 0028 — Faster and cheaper tickets on a big imported project

Status: accepted (extends ADR 0027; one measurement on Endless Sky)

## Context

Round 2 of the Endless Sky benchmark (a "ships destroyed" statistic; methods A, B, C on the same task)
gave C — Karkas's agents in mode mcp, served by a fresh session with tier subagents — 24 minutes and
$0.66, against 13 min / $0.41 for Claude Code alone. The models worked ~3 minutes of it. Measured:

- **Index updates: ~10 of 24 minutes.** Six updates of 90-107 s each (the ticket's start, the
  approval, each MCP call that updates the index). `Schema::links()` builds every link anew and was
  called per block (allowed_api_blocks with recursion over the leaves of groups, block_test_hash,
  leaf_flow_order per group); then D09 compared every call with every exported symbol, and include
  resolution tried 2 × 318 empty api/impl roots for every include.
- **Builds: ~9 minutes.** Every worker and every verification ran the release build with LTO
  (3-5 minutes each, ~3.5 of them the link).
- **Money: $0.63 of $0.66 in the coordinating Opus session**, 61% of it the 1-hour cache writes of new
  content: the orchestrator viewed the big groups `flight`, `data`, `ui` (~157K characters in two
  turns of round 1). The workers on Haiku cost $0.03.
- Worker turns of 80-110K characters exceeded the MCP output limit; Claude Code saved them to files that
  the subagents read with grep in pieces.

## Decision

1. **Links once.** `allowed_api_blocks`, `block_test_hash` and `ordered_children` take links (or the
   flow order) computed once by a caller that loops over blocks; D09 looks calls up in a name → owners
   map; include resolution tries only api/impl roots that hold files and matches imported files by
   name. Endless Sky: an index update 90 s → 5 s, `karkas check` the same.
2. **Compact views of big groups.** Past 8 internal links, calls between two blocks share a line; past
   24 external rows only the 20 heaviest pairs are listed with a count of the rest; an L2 view lists the
   20 most used of more than 24 neighbors. Small groups (the wavkit goldens) look as before. Top groups of
   Endless Sky: 3-4.5K characters.
3. **Likely blocks for the orchestrator.** Its first decision gets the 8 blocks `locate` finds for the
   words of the ticket (files and functions included); the prompt says to start there and view a whole
   group only when none fits.
4. **Smaller worker turns of imported blocks.** The headers of linked blocks get their own 20 KB budget
   and appear only with declarations the block uses (the most called blocks first); the rest is one line
   of names. An L2 view keeps the 20 most used neighbors.
5. **A compile check instead of builds.** `ITestService::compile_check(files)`: the project's
   compile_commands.json ([project] compile_commands, or found in the root / build folder / one folder
   under it, the release entry preferred) gives each file's command, run with `-fsyntax-only` (no object,
   no link; Endless Sky's Engine.cpp in ~8 s). `build()` of an imported block's worker and the check after
   its report compile the sources of its card. The project's full build and tests run once, after the last
   block of a feature (or after a bug's block); when it fails, the block whose file the first error names
   gets the errors as its task (twice at most), then the human.
6. **The coordinator.** The MCP instructions say to wait with kk_agent_next(wait_s=300) and, with
   auto_approve, not to look up kk_changes (C's coordinator spent 3 minutes of index updates on it).
   The orchestrator of an imported project may run on tier medium (`[mcp.roles] orchestrator = "medium"`,
   the hypothesis of spec 9.1), with the coordinating session on that model.

## Consequences

- Faster for B too (kk_check, kk_changes and every MCP call update the index).
- A header change of one block that breaks another block's file is found at the full build after the
  last block, not after the block; the repair goes back to the owner of the file.
- Locate is a heuristic: a likely block that does not fit costs the orchestrator a view, not a wrong
  dispatch.

## Measured

Endless Sky round 2 run again with these changes (C2: the coordinating session and the orchestrator on
Sonnet 5.5, the workers on Haiku 5.5, one run): closed done in **10.1 min and $0.23** — C before: 23.9 min
and $0.66; Claude Code alone (A): 13.4 min and $0.41; with Karkas as the map (B): 14.8 min and $0.63.

- The orchestrator decided in 22 s on 12.6K input tokens (before ~3.5 min, 53.6K); index updates took
  3-6 s (before 90-107 s); the two workers took 53 s and 45 s (before 6.5 and 4.5 min); the one full build
  ~6 min, now the largest part.
- The Sonnet orchestrator put the new function into a note of the card, not a fn line, and gave the panel
  no call line; the panel's worker proposed them itself (approved in its run, seconds). Opus had written
  the complete change at once. One run each: quality differences need more runs to judge.

The same with the session and the orchestrator back on Opus (C2-opus, no `[mcp.roles]`): closed done in
**8.4 min and $0.54** (Opus $0.52, Haiku $0.02). The orchestrator decided in 17 s on 13.4K input tokens
and wrote the complete change (fn, call, notes); the workers took 54 s and 30 s (5 and 4 steps); the full
build ~4.3 min. So these changes alone took C from 23.9 to 8.4 minutes and from $0.66 to $0.54; the
medium orchestrator took the money further to $0.23. Of the Opus session's $0.52, $0.34 were 1-hour cache
writes of the coordinating session's own context and the turns (the orchestrator's work ~$0.06 by
Karkas's estimate), so the coordinator, not the agents, now sets the price.

Both builds were checked in the running game (results/round2-ships-destroyed/runtime-check): the counter
counts, saves, loads and shows; the game's 45 integration tests pass.
