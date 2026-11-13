# 0020 — Agents see the outcomes of earlier descents; no circles

Status: accepted (amends 10.4 path views, 11.3 escalation, 12.1 roles, 13.2/13.3 tickets)

## Context

A test with DeepSeek models only on flappy-karkas (2026-11-12; report in
`flappy-karkas/karkas-deepseek-test-2026-11-12/REPORT.md`) found that most of the money went into
circles: 138 runs, $1.14, two successful outcomes. T-0004 ran the same descent 24 times, T-0007 ten
times, each ending `worker → needs_contract_change` with the same cause. The causes:

- The orchestrator's path view (`at=root`) kept only entries `at=root`: it never saw what the
  workers answered nor the `--- result` of the run, so it dispatched to the same leaf again. Its
  own suspects were erased (they are never "on the path" of the root).
- Nobody in a ticket could write `api/`: the worker sandbox refused it always, the architect is not
  part of a descent. After `approve C-NNNN` unfroze an api, the descent still looped. `expand` of an
  existing leaf rewrote its header blind (the architect has no read tool) and broke the build.
- Tunable numbers live in config's frozen `api/`: changing one constant was a contract change.
- A cause in a contract test (the tester's file) had no outcome; the worker said `not_here`, the
  navigator sent it back to the same leaf.
- `needs_contract_change` went to the human directly, not up to the orchestrator (11.3).
- A feature over several blocks closed after the first finished leaf.
- Escalation knew only the tier names local/cheap/medium/strong.
- About 94 % of the output tokens of DeepSeek were hidden reasoning; the OpenAI-compatible provider
  ignored `effort` and dropped `reasoning_content`.

## Decision

Path views (kk_core, `path_view`):
- The agent's own earlier decisions keep their suspects; repeats of one decision (role, pick,
  outcome, model) fold into the latest with `repeats=N`; a repeated fact is shown once.
- Views of the root and of groups end with `--- earlier descents from here (oldest first)`:
  `tried <picks> -> <role> at <block>: <outcome> | <the fact with its cause>`, identical consecutive
  descents folded `(xN)`.
- Views of the root and of the path to the block where the last descent ended end with
  `--- last result` (the latest result block without cost and state).

Descent (kk_agents, `run_ticket`):
- A block that answered in this run (not_here, needs_info, needs_contract_change, done) is not
  sent the ticket again in the same run: `dispatch` refuses it with the answer, and the decision
  text lists the answered blocks.
- `needs_contract_change` without an applicable card diff goes up to the orchestrator with the
  worker's cause as a fact (11.3). The orchestrator proposes the change, sends another block, or
  calls the new tool `escalate(note)` — the ticket goes to the human with that note.
- A feature leaf that is done (and verified green) returns to the orchestrator, which dispatches the
  next block or closes with verdict `done`; the result lists every finished block. Bugs still close
  after the fix. At most 6 leaves and 8 orchestrator decisions per run.
- New worker status `needs_tester`: the tester of the block corrects the contract test with the
  worker's finding as its task (never reading impl/), then the tests verify it.
- The orchestrator's `propose_change` passes the same checks as a worker's proposal in the tool.
- Every result ends with `state <hash>` (cards, api/ hashes, frozen set). `karkas ticket run`
  refuses to start any model while a change of the ticket waits for approval, or when the last run
  ended with needs_contract_change / escalated / needs_tester and the state is unchanged (model or
  network failures excepted). `--force` overrides. `karkas ticket close <id> [--note]` closes by hand.
- The orchestrator's first decision text says whether the cards or api/ changed since the last
  result (its advice may be outdated), and that the human forced the run when `--force` was given.
  Found live: a stale "the api is broken, a human must fix it" made the orchestrator escalate after
  the api had been restored.
- An edited api/ (values or the card's declarations) is verified with the tests of every dependent
  (12.4). The program writes `hist` lines only into leaf and entry cards: a `hist` in a shared card
  is E08 and stopped every agent (found live when config was tuned).

api/ in tickets (kk_agents, sandbox):
- `ApiAccess`: a frozen api lets a worker change values of constants only (`api_shape`: comments
  dropped, constexpr initializers replaced; any other edit is refused); the frozen hash then follows
  the file (no D05). An api unfrozen by an approved change (the project has frozen apis, this block's
  is not) is brought in line with the card by the worker (`api=changed`; `approve api` freezes it).
  Outside tickets and for testers api/ stays read-only.
- A frozen api may also gain new constexpr constants (`api_values_or_new_constants`: every old
  declaration kept, new pieces only `constexpr ... = value`). A constant is not on the card (KSL does
  not list them) and cannot break a user; a tunable number that moves into config is a ticket, not a
  change for approval. Removed or changed declarations, new types, fields and functions stay refused.
- A worker reads the api/ of the blocks that use its own (`Sandbox::user_blocks`; read-only, not put
  into the context, listed in the task), so a supplier whose contract changes sees how it is called.
- `approve C-NNNN` of a ticket's change points to `karkas ticket run T-NNNN`.
- The architect gets the current api/ of the block (update in place, keep style and namespace) or,
  for a new leaf, a header of the project as the style sample; `expand` of a leaf with an api is an
  update job.

Models (kk_core, kk_llm, kk_agents):
- `[routing] escalate = ["a", "b"]`: the escalation chain for any tier names (workers, navigators).
- `[effort] <role> = "low"`: reasoning effort per role, beats the tier's effort. The
  OpenAI-compatible provider sends `reasoning_effort` when an effort is set, keeps
  `reasoning_content` in the journal (not sent back: the API drops earlier reasoning — the prompt
  tokens do not change with it) and counts `reasoning_tokens`.
- The tester's tier goes through `tier_for` (pins and [roles]) instead of a hard-coded `medium`.
- The schema tools offer `probes` only when the project has probe data.

Found live as well: a DeepSeek orchestrator thought for 8192 tokens (the output limit, effort low)
without a tool call; the loop then sent back an assistant turn with neither content nor tool calls,
which the API refused (HTTP 400). Such a turn is now left out of the request; after a cut (up to three
per run) the model gets its last thoughts back as text with "decide now", because the API drops hidden
reasoning and the model would start over. A lower output limit for the schema agents
(`[limits] max_output_tokens_routing`) exists but is off: in the test it also cut an orchestrator that
was about to decide. Clear words in the prompt (which answer fits which situation, a ready example of a
change of a frozen api) removed the long deliberations instead. The orchestrator prompt no longer forbids sending a block again across
runs (the engine forbids it within a run): an answer from an earlier run stands only while nothing
changed and the worker could not handle it (a contract-test flaw is now `needs_tester`).

Found while finishing T-0007/T-0010 (C-0002, C-0003 approved by the author):
- "api unfrozen" was read as "work waits here" for blocks unfrozen long ago and already in line with
  their cards (an orchestrator re-sent a done block; a worker searched its card for a pause that was
  never there and thought for 8192 tokens). `status` and the worker's task now say "behind its card"
  (D04) or "already in line (approve api freezes it)"; the note after `approve C-NNNN` names the blocks
  it unfroze.
- A worker's "other blocks need X" was followed although the approved change said the opposite
  ("pause stays in app"). The orchestrator of a feature now gets the approved changes of its ticket
  (id, blocks, reason) as the agreed scope; the prompt calls the workers' lists hints.
- `run_ticket` re-indexes first, so drift and statuses are those of the project as it is.

Small fixes: `ask` answers are cut on a code point and a word boundary (6 steps instead of 4);
`report` without status names the allowed values; journals keep the system prompt and the context;
karkas.log names the role (`ask`, not `worker`); test freshness includes the api/ hashes of linked
blocks (`block_test_hash`), so a neighbor's contract change marks results stale.

## Consequences

- Ticket result sections may contain `state`; older readers reject it (S03).
- More orchestrator calls per feature (one after each finished leaf): a few cheap steps instead of a
  ticket that closes half done.
- A human who wants another identical run must pass `--force`.
