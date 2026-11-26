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

`karkas bench run --scenario s3 --method B --model sonnet --auth subscription` with the nightly build, the
same hidden tests and settings as the baseline. Planned with 3 runs; measured with one (`--reps 1`, the
author's decision of 2026-11-26) against the existing data: 3 runs of main and 2 of nightly
(bench/results/compare-2026-11-26). One run is indicative, not a median. Compared:
- cost, time, model calls, Karkas calls, `kk_note_bug` calls and repeats, the share of shell reads that
  touch only other blocks (the analysis script of the baseline);
- hidden tests: 11/11 in every run (the anti-goal).

## Consequences

Measured on 2026-11-27 (bench/results/compare-2026-11-27-known; one run, Release build of b167bb8):

| | main, 3 runs | nightly, 2 runs | this ADR, 1 run |
|---|---|---|---|
| cost of the run | $0.558 [0.551–0.596] | $0.590–0.595 | $0.579 |
| sessions 2–5 (can use the tickets) | $0.412–0.449 | $0.398–0.427 | $0.369 |
| session 1 (no tickets yet) | $0.109–0.158 | $0.163–0.197 | $0.210 |
| time | 688 s [535–917] | 795–1192 s | 671 s |
| model calls / Karkas calls | 56–59 / 25–26 | 65 / 28–29 | 53 / 22 |
| `kk_note_bug` calls, of them repeats | 7–11, 3–7 (31 of 51 in all 5 runs) | 11–12, 7–8 | 6, 0 |
| shell output that reads only other blocks | 40% | 50% | 24% |
| hidden tests | 11/11 ×3 | 11/11 ×2 | 11/11 |

- Where it can act, it works: sessions 2–5 cost less than in any of the five baseline runs (−14% against
  their median $0.427), and none of them recorded a known bug again (baseline sessions 2–4: 1–4 notes each,
  mostly repeats). Session 5 fixed its own bug although it was known (T-0002, recorded by session 1). It also
  found a different bug of the same block (`WavWriter::close` ignores write errors — real, latent in wavkit):
  the first call got T-0002 as the answer, the second, with `distinct: true`, recorded it — the price of
  deduplicating by block is one more call for a second bug.
- The run as a whole is not cheaper: session 1 cannot use tickets (none exist yet) and cost $0.210, above the
  baseline's sessions 1; with one run this is not separable from noise. It still reads other blocks' code to
  describe the bugs it records (35k characters of shell output; sessions 2–5: 9–18k) — the target of the next
  step (precise context for the bug's block).
- Changed after the measurement (from the final review), not measured: paths inside longer paths and files
  of imported blocks name a block; the answer of `kk_note_bug` lists every open ticket of the block; the
  last line of the `kk_run_tests` sections. The first two do not occur in s3; the third adds one line.
- Limit: in projects with their own build (imported, ADR 0009) `kk_run_tests` has no results per block, so
  the sections do not appear there; `kk_note_bug` deduplicates there through the listed files.
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
