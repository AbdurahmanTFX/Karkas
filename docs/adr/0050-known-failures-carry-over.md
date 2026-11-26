# 0050 — Known failures carry over between agent sessions

Status: accepted (branch nightly)

## Context

Method B of the benchmarks (Claude Code with the Karkas MCP server, scenario s3 on wavkit: five injected
bugs, one Claude Code session per bug report) was measured on 2026-11-26 (bench/results/compare-2026-11-26,
3 runs on main, 2 on nightly; prices fitted from the sessions' own costs: cache write $3.98/M, cache read
$0.10/M, output $10.07/M):

- a bug costs ~$0.11 and 8 model calls; ~60% of the money is context written into the cache (~18k tokens
  per bug), ~24% output, ~13% re-reading the cache;
- ~9k of those 18k tokens are code read through the shell (`cat`, `sed`, `grep`): 44% of it belongs only to
  other blocks than the bug's, 17% to the bug's block and others, 23% to the bug's block alone;
- `kk_note_bug` is called ~2 times per bug, and 61% of those calls (31 of 51) record a bug an earlier session
  of the same run had already recorded (the byte order of `io.writer` four times in one run).

The cause: `kk_run_tests` runs every test, so each session sees the failures of the other injected bugs;
the agent reads their code, recognises the bugs and records them again. The knowledge exists — `kk_note_bug`
creates an open bug ticket in `.karkas/tickets` — but nothing shows it to the next session. Karkas's own
principle P10 (knowledge persists, agents do not) is not carried through to these two tools.

This ADR is the first of two steps chosen by the author (the second: precise context for the bug's block,
its own ADR after this one is measured).

## Goals, non-goals, anti-goals

Goals
- A failure that already has an open bug ticket is shown as known, with the ticket, where the agent sees the
  failure (`kk_run_tests`), so it does not read that block's code to rediscover it.
- `kk_note_bug` does not create a second open ticket for a block that already has one; it names the existing
  ticket instead.
- Measured on s3 (method B, sonnet, subscription, 3 runs) against the baseline of 2026-11-26.

Non-goals
- No automatic closing of tickets (a ticket stays "for the user", as today).
- No change of Karkas's own engine (method C), the GUI or the CLI output of `karkas test`; the helper below
  is shared, so they can adopt it later.
- No precise context for the bug's block (the next ADR).

Anti-goals
- The agent must not skip its own task because its failure is "known": in s3 the fifth session's bug
  (`io.writer`) was recorded by earlier sessions. The text says "if this is your task, fix it". A run where
  the hidden tests drop below 11/11 fails this ADR.
- A distinct second bug in the same block must still be recordable.
- No model call: matching failures to tickets is deterministic (P7).

## Approaches considered

1. Mark known failures in `kk_run_tests` and deduplicate `kk_note_bug` by block (chosen): small, uses data
   that exists, reaches the agent exactly where it decides to investigate.
2. List the open tickets in the prompt or the guide at session start: the agent would read them before it
   knows which failures matter; more tokens on every session, also when nothing fails.
3. Run only the bug's block's tests by default: hides real regressions in other blocks the fix may cause.

## Decision

### Which block a ticket is about

`core::ticket_block(const Ticket&, const Schema&) -> std::string` (empty when none):
1. a fact `block <id>` (written by `kk_note_bug` from now on);
2. otherwise, in the facts `found at …` and then the symptom: a path under a block's folder (`dir`) or one
   of the files an imported block lists (`files`, ADR 0009), also inside a longer path (absolute, `./…`);
   the longest such path wins;
3. otherwise the longest block id of the schema that occurs as a whole word outside a path (`io.writer`,
   `stream.ring`). Paths come first because `blocks/dsp/echo/…` contains the id `dsp` of the group.
4. when neither the `found at` facts nor the symptom name a block, the other facts in order (a human's
   `karkas debug --fact "contract stream.ring/no_drop FAIL"`).

### Open bug tickets of a block

`core::open_bugs(const TicketStore&, const Schema&) -> std::map<block, std::vector<Ticket>>`: tickets of kind
bug that are not closed, by `ticket_block` (those without a block are left out), in id order.

### `kk_run_tests`

After the summary, when at least one block fails:
```
--- known: these failures already have an open ticket
io.writer  T-0003  PCM16 output samples are written with bytes swapped
--- no ticket: investigate them only if they are your task
dsp.echo
If one of the known failures is the bug of your task, fix it; otherwise leave it.
Any of them that started failing after your change is yours to fix.
```
A failing block with several open tickets lists each. A block of a failing integration test (a group) uses
the tickets of the blocks under it. The line "If one of the known failures…" comes with the known section;
the last line always comes (added after the final review: a regression the agent's own change causes must
not be left as "not my task", the reason approach 3 was rejected).

### `kk_note_bug`

- The block is resolved from `where` (and `summary`) with the rules above; the new ticket gets the fact
  `block <id>`.
- When the block already has an open bug ticket and the call does not set `distinct: true`: nothing is
  created; the answer is "already noted as T-0003 (<symptom>). Name it in your report. If this is a
  different bug in io.writer, call again with distinct=true."
- When the block has several open bug tickets (after a `distinct: true`), the answer lists all of them:
  "already noted in stream.ring: T-0001 (…); T-0002 (…). If one of them is yours, name it in your report; if
  this is a different bug, call again with distinct=true." (added after the final review: naming only the
  first would make a repeat of the second look like a new bug).
- `where` that names no block: created as today (cannot be deduplicated).

### Texts

The descriptions of `kk_run_tests` and `kk_note_bug`, `templates/prompts/mcp_instructions.txt` and the
generated guide (`karkas gen guide`) say in one line each: known failures carry their ticket; investigate
them only if they are your task; `kk_note_bug` answers with the existing ticket.

### Measurement

`karkas bench run --scenario s3 --method B --model sonnet --auth subscription --reps 3` with the nightly
build, the same hidden tests and settings as the baseline; compared with the 3 runs of main:
- cost, time, model calls, Karkas calls, `kk_note_bug` calls and repeats, the share of shell reads that
  touch only other blocks (the analysis script of the baseline);
- hidden tests: 11/11 in every run (the anti-goal).

## Consequences

- Expected on s3: most repeated notes disappear and most reads of other blocks' code go; the exact gain is
  what the measurement shows. In everyday work the gain depends on how often known failing tests and several
  sessions on one project occur; on s3 they are maximal (five injected bugs at once).
- Tickets now carry `block <id>`; old tickets are matched by their text.
- One more parameter of `kk_note_bug` (`distinct`), and two more sections in the answer of `kk_run_tests`.

## Left to the builder

- Where the two helpers live (`libs/core` next to `TicketStore`, or `libs/agents`), and their tests.
- How the failing blocks are taken from the result of `TestService::build_and_test` (structured, not parsed
  from text).
- The exact wording of the lines, within the meaning above.
