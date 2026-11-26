# Known Failures Carry Over — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `kk_run_tests` shows which failures already have an open bug ticket, and `kk_note_bug` answers with the existing ticket instead of a duplicate; then measure the effect on bench s3 (method B) against the baseline of 2026-11-26.

**Architecture:** Pure helpers next to `TicketStore` in `kk_core` resolve the block a ticket or a text names, collect open bug tickets by block, and format the two "known / no ticket" sections. The MCP server uses them in `kk_note_bug`; the CLI's MCP wiring (`apps/cli`) appends the sections to the `kk_run_tests` answer. Texts for agents say one line each.

**Tech Stack:** C++23, CMake + Ninja, doctest, golden files; Claude Code bench (`karkas bench`).

**Spec:** `docs/adr/0050-known-failures-carry-over.md`

## Global Constraints

- C++23; errors as `kk::Result`; no exceptions across module boundaries; no new dependency.
- No model call anywhere in this change (P7).
- `karkas test` (the CLI command) prints exactly what it printed before; only the MCP answer of `kk_run_tests` changes.
- Tickets are never closed or edited by this change; `kk_note_bug` only adds the fact `block <id>` to tickets it creates.
- Doctest case names contain no `;`.
- Golden files are regenerated with `KK_UPDATE_GOLDEN=1` and the diff is read before the commit.
- Branch `nightly`; one commit per task, message ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build: `cmake --build --preset dev`; tests of a library: `ctest --preset dev -L <lib>`; all fast tests: `ctest --preset dev-fast`.

## Review Focus

- The bug of the current task is itself a known failure (s3, fifth session: `io.writer` was recorded earlier): the text must invite fixing it when it is the task — covered by the wording test in Task 1 and checked in the measurement (Task 4: hidden 11/11).
- A path in `where` must win over a block id that appears inside the path (`blocks/dsp/echo/impl/echo.cpp` is `dsp.echo`, not the group `dsp`) — test in Task 1.
- A ticket about a leaf under a group whose integration test fails counts for that group — test in Task 1.
- Closed tickets and tickets of other kinds (feature, change) are never "known" — test in Task 1.
- A `where` that names no block still creates a ticket (no deduplication possible) — test in Task 2.

---

## File structure

```
libs/core/include/kk/core/tickets.hpp   + block_named_in, ticket_block, open_bugs, known_failures_text
libs/core/src/tickets.cpp               their bodies
libs/core/tests/tickets_test.cpp        tests of the helpers
libs/mcp/src/server.cpp                 kk_note_bug: block fact, deduplication, `distinct`; texts of two tools
libs/mcp/tests/mcp_test.cpp             note_bug tests
tests/golden/mcp/session.out.jsonl      tools/list texts (regenerated)
apps/cli/cmd_test.cpp                   + mcp_run_tests_text(root, block)
apps/cli/cmd_mcp.cpp                    o.run_tests uses mcp_run_tests_text
templates/prompts/mcp_instructions.txt, templates/target/{karkas_guide,karkas_guide_full,claude_agent}.md
docs/adr/0050-known-failures-carry-over.md, docs/progress.md
```

---

### Task 1: Core helpers — which block, open bugs, the two sections

**Files:**
- Modify: `libs/core/include/kk/core/tickets.hpp`, `libs/core/src/tickets.cpp`, `libs/core/tests/tickets_test.cpp`, `docs/adr/0050-known-failures-carry-over.md`

**Interfaces:**
- Produces (namespace `kk::core`, in `tickets.hpp`):
  ```cpp
  // The block a free text names: the deepest block whose folder (Block::dir) occurs as a path in the text;
  // else the longest block id that occurs as a whole word (the character before is not [a-z0-9_./],
  // the one after is not [a-z0-9_]); "" when none.
  std::string block_named_in(std::string_view text, const Schema& schema);
  // A ticket's block: a fact "block <id>" (an existing block); else block_named_in of each "found at …"
  // fact in order, then of the symptom; "" when none.
  std::string ticket_block(const Ticket& t, const Schema& schema);
  // Bug tickets whose status is not "closed", by ticket_block; tickets without a block are left out;
  // each list in id order.
  Result<std::map<std::string, std::vector<Ticket>>> open_bugs(const TicketStore& store, const Schema& schema);
  // The sections kk_run_tests adds (ADR 0050); "" when `failing` is empty. A failing group uses the
  // tickets of the blocks under it (Schema::is_under).
  std::string known_failures_text(const std::vector<std::string>& failing,
                                  const std::map<std::string, std::vector<Ticket>>& bugs, const Schema& schema);
  ```
- Text of `known_failures_text` (exact; sections left out when empty, block ids sorted):
  ```
  --- known: these failures already have an open ticket
  <block>  <T-NNNN>  <symptom>
  --- no ticket: investigate them only if they are your task
  <block>
  If one of the known failures is the bug of your task, fix it; otherwise leave it.
  ```
  (the last line only when the known section is present).

- [ ] **Step 1: Write the failing tests** in `libs/core/tests/tickets_test.cpp` (the schema of `tests/fixtures/wavkit` via `load_schema(test::fixture("wavkit"))`; tickets made in a `test::TempDir` with `TicketStore::create`)

```cpp
TEST_CASE("tickets: a path names the deepest block, not an id inside the path") {
  auto s = load_schema(test::fixture("wavkit")); REQUIRE(s);
  CHECK(block_named_in("blocks/dsp/echo/impl/echo.cpp:24", *s) == "dsp.echo");
  CHECK(block_named_in("io.writer, blocks/io/writer/impl (WavWriter::write)", *s) == "io.writer");
  CHECK(block_named_in("Ring buffer corrupts frames in stream.ring at wraparound", *s) == "stream.ring");
  CHECK(block_named_in("contract stream.ring/no_drop FAIL", *s) == "stream.ring");
  CHECK(block_named_in("the output sounds wrong", *s).empty());
}
TEST_CASE("tickets: open bugs by block leave out closed tickets and other kinds") {
  test::TempDir d; auto s = load_schema(test::fixture("wavkit")); REQUIRE(s);
  TicketStore store(d.path());
  REQUIRE(store.create(TicketKind::Bug, "agent", "bytes swapped", {"found at blocks/io/writer/impl/writer.cpp:40"}));
  REQUIRE(store.create(TicketKind::Bug, "agent", "ring drops frames", {"block stream.ring"}));
  REQUIRE(store.create(TicketKind::Feature, "human", "add reverb to dsp.chain", {}));
  auto closed = store.create(TicketKind::Bug, "agent", "mix not clipped", {"found at mix"}); REQUIRE(closed);
  closed->status.value = "closed";
  REQUIRE(store.save(*closed));
  auto bugs = open_bugs(store, *s); REQUIRE(bugs);
  CHECK(bugs->size() == 2);
  CHECK(bugs->at("io.writer")[0].symptom.value == "bytes swapped");
  CHECK(bugs->contains("stream.ring"));
  CHECK_FALSE(bugs->contains("mix"));
}
TEST_CASE("tickets: known failures name their tickets and the rest stay unexplained") {
  test::TempDir d; auto s = load_schema(test::fixture("wavkit")); REQUIRE(s);
  TicketStore store(d.path());
  REQUIRE(store.create(TicketKind::Bug, "agent", "bytes swapped", {"block io.writer"}));
  REQUIRE(store.create(TicketKind::Bug, "agent", "gain applied twice", {"block dsp.chain"}));
  auto bugs = open_bugs(store, *s); REQUIRE(bugs);
  auto text = known_failures_text({"io.writer", "dsp", "dsp.echo"}, *bugs, *s);
  CHECK(text.find("--- known: these failures already have an open ticket\n") != std::string::npos);
  CHECK(text.find("io.writer  T-0001  bytes swapped\n") != std::string::npos);
  CHECK(text.find("dsp  T-0002  gain applied twice\n") != std::string::npos);   // a group: tickets under it
  CHECK(text.find("--- no ticket: investigate them only if they are your task\ndsp.echo\n") != std::string::npos);
  CHECK(text.find("If one of the known failures is the bug of your task, fix it; otherwise leave it.") != std::string::npos);
  CHECK(known_failures_text({}, *bugs, *s).empty());
}
```

- [ ] **Step 2: Run to see them fail** — `cmake --build --preset dev --target kk_core_tests` → compile errors (functions not declared).
- [ ] **Step 3: Implement** the four functions in `libs/core/src/tickets.cpp`.
- [ ] **Step 4: Run** — `ctest --preset dev -L core` → all pass.
- [ ] **Step 5: Amend the ADR** (rule order): in "Which block a ticket is about", the order becomes: 1) fact `block <id>`; 2) a path under a block's folder (the deepest); 3) the longest block id as a whole word, where a word inside a path does not count. Reason: `blocks/dsp/echo/…` contains the id `dsp` of the group. Status stays `proposed` until Task 3.
- [ ] **Step 6: Commit** — `git commit -m "tickets: the block a ticket names, open bugs by block, known failures (ADR 0050)"`

---

### Task 2: `kk_note_bug` — block fact, deduplication, `distinct`

**Files:**
- Modify: `libs/mcp/src/server.cpp` (tool schema at the `kk_note_bug` entry ~line 91; handler ~line 432), `libs/mcp/tests/mcp_test.cpp`, `tests/golden/mcp/session.out.jsonl`

**Interfaces:**
- Consumes: `core::block_named_in`, `core::open_bugs` (Task 1); `idx_.schema()` (the server updates the index before each tool call).
- Tool schema: new property `distinct` (boolean): "true: a second, different bug in a block that already has an open bug ticket". Description gains: "If the block already has an open bug ticket, it answers with that ticket instead of creating another."
- Behaviour: block = `block_named_in(where)`, else `block_named_in(summary)`. When the block is not empty and `distinct` is not true and `open_bugs` lists a ticket for it → no ticket; answer (not an error):
  `already noted as T-NNNN (<symptom>). Name it in your report. If this is a different bug in <block>, call again with distinct=true.\n`
  Otherwise create as today, appending the fact `block <id>` when the block is known.

- [ ] **Step 1: Write the failing tests** (update the existing case and add two)

```cpp
// in "mcp: kk_note_bug records a bug found on the way as an open ticket":
REQUIRE(t->facts.size() == 3);
CHECK(t->facts[0].text == "found at blocks/dsp/echo/impl/echo.cpp:24");
CHECK(t->facts[2].text == "block dsp.echo");

TEST_CASE("mcp: kk_note_bug answers with the open ticket of the block instead of a duplicate") {
  McpFixture f;  // holds T-0001, a bug about stream.ring
  mcp::Server server(*f.idx, {});
  auto call = /* as in the case above */;
  auto r = call({{"summary", "ring buffer loses frames at wraparound"}, {"where", "blocks/stream/ring/impl/ring.cpp:30"}});
  CHECK(r["isError"] == false);
  CHECK(r["content"][0]["text"].get<std::string>().starts_with("already noted as T-0001"));
  CHECK_FALSE(core::TicketStore(f.p.path()).load("T-0002"));
  auto again = call({{"summary", "fill() can exceed capacity"}, {"where", "stream.ring"}, {"distinct", true}});
  CHECK(again["content"][0]["text"].get<std::string>().starts_with("noted T-0002"));
}
TEST_CASE("mcp: kk_note_bug without a known block still records the bug") {
  McpFixture f;
  mcp::Server server(*f.idx, {});
  auto call = /* as above */;
  auto r = call({{"summary", "the output sounds wrong"}, {"where", "somewhere in the pipeline"}});
  CHECK(r["content"][0]["text"].get<std::string>().starts_with("noted T-0002"));
}
```

- [ ] **Step 2: Run to see them fail** — `cmake --build --preset dev --target kk_mcp_tests && ctest --preset dev -L mcp` → the three cases fail (facts size 2; a duplicate is created).
- [ ] **Step 3: Implement** in `server.cpp`.
- [ ] **Step 4: Regenerate the golden session** — `KK_UPDATE_GOLDEN=1 ctest --preset dev -R "mcp/mcp: golden JSON-RPC session"`; `git diff tests/golden/mcp/` shows only the `kk_note_bug` description and the new `distinct` property.
- [ ] **Step 5: Run** — `ctest --preset dev -L mcp` → all pass.
- [ ] **Step 6: Commit** — `git commit -m "mcp: kk_note_bug answers with the open ticket of the block (ADR 0050)"`

---

### Task 3: `kk_run_tests` sections, texts for agents, ADR accepted

**Files:**
- Modify: `apps/cli/cmd_test.cpp` (+ `mcp_run_tests_text`), `apps/cli/cmd_mcp.cpp:114` (use it), `libs/mcp/src/server.cpp:158` (description), `templates/prompts/mcp_instructions.txt`, `templates/target/karkas_guide.md:20-21`, `templates/target/karkas_guide_full.md:~139`, `templates/target/claude_agent.md:32,38`, `docs/adr/0050-known-failures-carry-over.md` (Status: accepted), `tests/golden/mcp/session.out.jsonl` (description)

**Interfaces:**
- Consumes: `core::open_bugs`, `core::known_failures_text` (Task 1); `agents::TestService::build_and_test` → `TestRun{build_ok, blocks (runner::BlockTests: block, counts, build_failed, failures), text}`.
- Produces: `std::string mcp_run_tests_text(const fs::path& root, const std::string& block);` — `run_tests_text` plus, when the build succeeded, `known_failures_text(failing, open_bugs(store, schema), schema)` where `failing` = blocks of the run with `build_failed`, any `failures`, or `pass < total` in a kind. `run_tests_text` itself is unchanged (`karkas test` prints the same).
- Texts (one line each, meaning fixed by the ADR):
  - `kk_run_tests` description: "…; failures that already have an open bug ticket are listed as known."
  - `mcp_instructions.txt`: "kk_run_tests lists failures that already have an open ticket as known: investigate them only if they are your task."
  - `karkas_guide.md` line 21: "- Done means: kk_check shows 0 errors and kk_run_tests passes, except the failures it lists as known (they have open tickets; fix one only if it is your task)." and line 20 adds "(it answers with the existing ticket when the block already has one)".
  - `karkas_guide_full.md` and `claude_agent.md`: the same two meanings in their wording.

- [ ] **Step 1: Write the failing check** — a doctest is not possible without a real build here, so the wiring is checked by hand in Step 4; the formatting itself is covered by Task 1.
- [ ] **Step 2: Implement** `mcp_run_tests_text`, switch `cmd_mcp.cpp`, edit the texts.
- [ ] **Step 3: Regenerate the MCP golden** (description of `kk_run_tests` only appears when a runner is wired — if the golden session has no runner, nothing changes; read the diff) and run `ctest --preset dev -L mcp`.
- [ ] **Step 4: Check the wiring by hand** — in the session scratchpad: copy `tests/fixtures/wavkit`, apply the bug patches `bench/wavkit/bugs/writer_byte_order.json` and `echo_feedback.json` (replace `old` by `new` in each `file`), then run `karkas mcp --project <copy>` with `initialize`, `tools/call kk_note_bug {"summary": "bytes swapped", "where": "blocks/io/writer/impl/writer.cpp:40"}` and `tools/call kk_run_tests {}`.
  Expected: the answer of `kk_run_tests` ends with the known section naming `io.writer  T-0001  bytes swapped`, the no-ticket section naming `dsp.echo`, and the "If one of the known failures…" line.
- [ ] **Step 5: ADR status** → `accepted (branch nightly)`.
- [ ] **Step 6: Run all fast tests** — `ctest --preset dev-fast` → all pass.
- [ ] **Step 7: Commit** — `git commit -m "mcp: kk_run_tests lists known failures; texts for agents (ADR 0050)"`

---

### Task 4: Measurement on s3 (method B) against the baseline

**Files:**
- Modify: `docs/adr/0050-known-failures-carry-over.md` (Consequences: measured numbers), `docs/progress.md` (new section)
- Results: `bench/results/compare-2026-11-27-known/` (ignored by git)

- [ ] **Step 1: Build the binary used by the bench** — `cmake --preset headless && cmake --build --preset headless --target karkas` (Release, as the baseline).
- [ ] **Step 2: Prepare the bench folder if the scratchpad lost it** — a folder with `karkas.toml` (`[bench] hidden_tests = "<copy of bench/wavkit/hidden_tests.example>"`) and a copy of `bench/wavkit` without `hidden_tests.example` (as on 2026-11-26).
- [ ] **Step 3: Run** — from that folder: `<headless karkas> bench run --scenario s3 --method B --model sonnet --auth subscription --reps 3 --bench-dir <copy>/wavkit --reference tests/fixtures/wavkit --results <results-known> --work <work-known>`; then `bench report --results <results-known>` (in the background; ~40 min).
- [ ] **Step 4: Analyse** with the scripts of the baseline (tokens by kind with the fitted prices, shell reads by block, `kk_note_bug` repeats) for `main` (baseline) and `known`.
  Expected: hidden 11/11 in all 3 runs (the anti-goal). Report the medians: cost, time, model calls, Karkas calls, note calls and repeats, share of reads that touch only other blocks.
- [ ] **Step 5: Record** — copy the results to `bench/results/compare-2026-11-27-known/`; write the numbers into the ADR's Consequences and a section in `docs/progress.md`.
- [ ] **Step 6: Commit** — `git commit -m "docs: ADR 0050 measured on s3 (method B)"`
