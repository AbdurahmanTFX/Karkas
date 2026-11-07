# Karkas — progress by stage

## Stage 0 — repository skeleton
- Folder layout of 19.1, CMakeLists + CMakePresets (dev, release, asan, headless),
  cmake/{Dependencies,ThirdPartyTargets,Warnings,Sanitizers,KarkasHelpers,EmbedFiles}.cmake.
- Dependencies: system packages first (sqlite, curl, fmt, spdlog, toml++, xxhash,
  tree-sitter, glfw); FetchContent with pinned hashes for nlohmann_json, doctest,
  CLI11, reproc, tree-sitter-cpp, ImGui docking, imgui-node-editor, ImGuiColorTextEdit.
- `karkas --version`, CLAUDE.md, .clang-format, .clang-tidy, .gitignore.
- compile_commands.json is symlinked into the repo root by the dev preset.

## Stage 1 — data model and KSL (kk_core)
- Generic record lexer/parser (4.5) with file:line:col errors (S01–S07),
  typed records (project, block, ticket, report, change), canonical writer (4.8),
  comments preserved as trivia, checks E01–E08 and W01–W06 (4.6), `karkas check`.
- Tests: 71 test cases in kk_core_tests; every E and W code has a dedicated test;
  round trip of every fixture .ksl file is byte-identical.

## Stage 2 — index and views (kk_index)
- DDL of 7.1 (plus test_results, facts tables and runs.journal / probes.link /
  tickets.title columns, see docs/adr/0003), WAL, one writer.
- Full and incremental indexing by xxh3 fingerprints; code facts cached by content hash;
  runs/probes/tickets/approvals/test results loaded from .karkas/.
- Queries of 7.3, lifted links (3.6), L0/L1/L2 views with golden files
  (tests/golden/views), token budget checks (estimate) L0 ≤ 800, L1 ≤ 600, L2 ≤ 1200.
- `karkas index [--watch] [--full]`, `karkas view`, `karkas query`.
- Karkas describes itself: project.ksl + block.ksl in libs/* and apps/*;
  `karkas view` works on this repository.
- Timing (debug build, synthetic 44 blocks / ~20k lines, no code analyzer):
  full 31 ms, unchanged 15 ms, one file changed 28 ms.

## Stage 3 — MCP server (kk_mcp)
- JSON-RPC 2.0 over stdio, one message per line: initialize (version negotiation,
  capabilities.tools, serverInfo, instructions), notifications/*, ping, tools/list,
  tools/call; parse/invalid-request/method/params errors.
- Tools of 15.3: kk_overview, kk_view, kk_children, kk_neighbors, kk_dependents,
  kk_path, kk_search, kk_history, kk_status, kk_probes, kk_drift, kk_ticket_view,
  kk_ticket_append, kk_run_tests (only when a test pipeline is wired in).
- Ticket store in kk_core: append-only files, lock files, path view (10.1, app. D),
  budget folding (10.4).
- `karkas mcp [--project <dir>]`: stdout is protocol only, logs go to stderr/.karkas/karkas.log.
- Golden session: tests/golden/mcp/session.{in,out}.jsonl.
- Not verified here (manual): MCP Inspector and `claude mcp add karkas -- karkas mcp --project <dir>`.

## Stage 4 — model clients and accounting (kk_llm, kk_metrics)
- Anthropic Messages API client: tools/system/messages, cache_control markers
  (max 4, ttl 5m/1h), output_config.effort, optional server-side refusal fallback,
  usage incl. cache_creation/cache_read, verbatim replay of assistant blocks
  (thinking signatures), retries on 429/5xx/529 and transport errors with
  exponential backoff honouring retry-after (≤ 5 attempts), no retry on 400,
  count_tokens endpoint. API version header 2026-10-21 (docs/adr/0005).
- OpenAI-compatible client (POST {base_url}/chat/completions, function calling).
- Record/replay at HTTP level (RecordingTransport never stores keys; ReplayTransport,
  optional strict request matching); KARKAS_LLM_RECORD / KARKAS_LLM_REPLAY.
- kk_metrics: run journals .karkas/runs/*.jsonl with an active marker, cost formula of
  16.2 with per-model cache price overrides, unpriced cloud model = error, local = 0,
  `karkas cost [--by] [--since]`, `karkas llm ping|tokens`.
- Replay tests: plain answer, tool use, 429 (retry-after), 500/529/transport error, 400,
  persistent 529, request shape. Not run here: the live smoke call (`karkas llm ping`)
  and the cache_read growth check — they need ANTHROPIC_API_KEY and spend money.

## Stage 5 — code analysis, generation, runner (kk_code, kk_runner)
- tree-sitter-cpp: includes (quoted/system), declarations of api/ (class, struct, enum,
  alias, functions, methods with parameter counts, pure virtual flag, signatures),
  definitions and called names in other files; facts cached by content hash in the index.
- Drift checks D01–D09 (D01–D05, D07 errors; D06, D08, D09 warnings; D04 parameter count
  and stale CMakeLists are warnings). Each has a deliberately broken variant of the
  reference project in libs/code/tests/drift_test.cpp.
- Generation: `karkas gen cmake|mocks|probe|skel|all`, `karkas init`. CMakeLists.txt per 5.5
  (leaf/shared → STATIC or INTERFACE, PRIVATE links from the consumer cards, groups →
  INTERFACE, entry → executable, tests per leaf with labels id+ancestors, integration tests
  per group, cmake/user.cmake), karkas-hash line for D07. Mocks implement the neighbor
  interfaces with call records and on_<method> handlers. Probe header KK_PROBE (lock-free
  ring + background flush, KK_PROBES=0 for zero cost).
- Runner: reproc++ with deadline, terminate/kill, cleaned environment (whitelist, no
  *KEY*/*TOKEN*/*SECRET*), cmake configure/build, `ctest -V` parsing into per-block
  contract/unit/integration counts and failing test cases, log compression (errors only,
  deduplicated, ≤ 40 lines).
- The reference project tests/fixtures/wavkit now has real code (13 code blocks), builds
  with the generated CMake, its 21 test cases pass, the program runs and writes probes;
  a leaf without a link fails to compile when including another leaf's api.
- Karkas checks itself: 0 errors (warnings: orphan support files, heuristic D09).

## Stages 6–7 — agents, tickets, routing (kk_agents)
- Role prompts in templates/prompts (appendix B + a KSL reference), overridable per
  project in .karkas/prompts/<role>.txt.
- Agent loop (9.4): tools → system (cache marker) → stable context (marker) → task;
  the marker moves to the newest tool results; limits: steps, tokens, timeout; one nudge
  when the model answers in prose; refusal handling; journal per run, runs table.
- Sandbox (9.5): '..', absolute paths, symlinks pointing outside, writes to api/ (with the
  needs_contract_change hint), unlinked api/ and other blocks' impl/ are refused; the
  tester never reads impl/; build/test run through kk_runner with a cleaned environment.
- Tools of all roles (9.3), router (11.2) with self-tuning (11.4), escalation (11.3):
  two failed attempts → next tier, budget/steps exhausted → next tier, strongest tier
  failed → ticket escalated; "fixed" with failing tests counts as failed.
- Tickets: descent root → navigators → leaf with path views (10.1), not_here (twice on a
  level → one level up, stronger navigator), needs_contract_change → pending @change,
  ask() (10.5), regression after a fix (12.4), hist line written by the program (3.10),
  result section with cost.
- 13.1: design (stop) → approve L0 → expand --all → approve api (freeze + gen probe, skel,
  mocks, cmake) → implement (testers, workers in parallel, composition root by a medium
  worker, integration testers for groups, final test run). @change approval applies the
  diff, unfreezes changed contracts and marks dependents needs_review.
- CLI: design, expand, approve, implement, test, task, debug, ask, ticket list|show|run,
  change list|show|reject.
- Tests (scripted model responses, no network): sandbox escapes one by one, loop limits,
  descent with trail lines as in 10.2, escalation cheap→medium→strong→human, not_here,
  contract change + approval, ask, tester without impl/, design flow, and a real build
  where the injected ring wraparound bug is found by descent and fixed (contract tests go
  from failing to passing). The orchestrator and navigators never receive code (checked on
  the recorded requests).
- Not verified here: "idea → working wavkit" with real models (needs an API key and money).
