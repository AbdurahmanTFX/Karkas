# 0017 — The cost of a Karkas session: lean start, compact answers, kk_locate

Status: accepted (deviation from spec 7.3–7.4 and 15.3)

## Context

The logs of the Blobby experiments (stream-json, tokens per turn) showed where Claude Code with
Karkas (method B) pays more than without it (A). On the Sonnet bug hunt B read 53% more cached
tokens for the same five one-line fixes:
- ~50%: a heavier start — every B session began ~3.2k tokens larger (the guide in CLAUDE.md,
  the MCP instructions, 19 tool names, 3 subagents), re-read in every turn;
- ~28%: large Karkas answers that stay in the context — kk_view averaged 2.8k tokens on the
  imported Blobby (L1 of the group `state`: ~6.3k, 309 lines of single external calls), against
  the 600/1200 budgets of the spec; kk_check printed all 97–142 warnings (2.3k);
- ~22%: 2–3 turns of orientation (ToolSearch, kk_overview, kk_view) before the search.
A Haiku session stopped before starting a one-line fix because "the full Karkas workflow (guide,
schema change, approvals, delegation)" looked too costly.

## Decision

- Two guides: a short one in CLAUDE.md (26 lines, ~400 tokens) that picks the path by the size of
  the task — a bug or a small change goes kk_locate → read → fix → kk_check/kk_run_tests without a
  schema change; a feature or a structural change reads the full method — and the full method
  returned by kk_guide (template karkas_guide_full.md). Shorter MCP instructions say the same.
- `Index::locate` / kk_locate / `karkas query locate <words>`: blocks that probably hold a
  symptom, ranked by its words in block names, purposes, file names and code symbols
  (camelCase/snake_case split, inflections but not plain prefixes, a small table of general
  software synonyms, inverse document frequency, the three strongest words decide); with files
  and the matching symbols at file:line.
- Views group repeated calls: L1 external calls between the same two blocks share one line
  (`-call-> b:f, g, h +9`), L2 shows callees with up to 6 signatures and callers with up to 4
  functions per neighbour; single links look as before (golden views of wavkit unchanged).
- kk_check is a summary: every error in full, warnings counted per code with two examples
  (`core::issues_summary`); `karkas check` still prints everything.
- Delegation: larger work only (new functions, a whole leaf, its tests); a few-line fix or work
  judged by eye stays with the session; hand out at once and wait instead of polling.

## Consequences

- On Blobby: L1 of `game` 6.1 KB instead of ~25 KB for `state`-sized groups; kk_check ~10 lines.
- kk_locate is a hint, not an answer: on the five bug reports of the hunt the culprit was in its
  top 3 for 3 of 5 and in the top 5 for 4 of 5 (a replay bug described by what the player sees
  in the replay points at the player, not at the recorder). The agent still reads the code.
