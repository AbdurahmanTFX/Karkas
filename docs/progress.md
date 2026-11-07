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
