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

## Stages 8–9 — GUI (kk_layout, karkas-gui)
- kk_layout: ILayoutEngine with a built-in Sugiyama engine (cycle removal, longest-path
  ranks, dummy nodes, barycentric crossing reduction, coordinates without overlaps;
  rankdir LR) and a Graphviz dot engine compiled when libgvc/libcgraph are found
  (not built here: Graphviz is not installed, docs/adr/0002). Layout cache by structure
  hash (.karkas/cache/layout), manual positions in .karkas/layout/<view>.json win.
  "All levels" lays groups out as boxes around their children.
- karkas-gui: Dear ImGui docking + GLFW + OpenGL 3.3 + imgui-node-editor +
  ImGuiColorTextEdit. Left: block tree with status dots; center: schema of the current
  level (pins = ports, lifted links, colors of 14.5, pulsing yellow while an agent runs,
  orange border = needs_review), breadcrumbs, double click group → enter, leaf → code;
  right: inspector (card, ports, invariants, history, files, model view) and read-only
  code view; bottom tabs: tickets (trail), agents (active and recent runs), build & tests,
  costs (by block/model/ticket, cache-read share), drift, changes (approve/reject).
- Editing (stage 9): add/remove block and port, drag a link (out → in writes
  "in ... from=", fn → "call"), delete a link, rename (folder moves, references follow),
  split, merge; every edit is shown as a KSL diff first, applied only if the schema checks
  pass, and can be undone (kk_agents edit ops, tested in libs/agents/tests/edit_test.cpp).
  Manual node moves are saved; "open in editor" runs editor_command detached.
- Threads: the index, layouts, file reads and edits run in a worker thread; the GUI thread
  only reads finished models. Live updates through inotify. Cyrillic font with glyph
  ranges, HiDPI content scale.
- Measured under Xvfb (debug build, software GL): a 44-block / ~20k-line project opens in
  ≈0.35 s with an existing index and ≈0.95 s from scratch; idle CPU 0% over 10 s.
  Screenshot: docs/screenshots/stream_level.png.

## GUI addition — settings, language, block sizes (docs/adr/0008)
- Menu "Karkas → Settings…": interface language (English / Русский, default from LANG),
  default block size (0.6–2.5), interface text size; saved to ~/.config/karkas/gui.json.
  All panels, menus, dialogs, tabs, kinds and statuses are translated; window ids are
  language independent ("Блоки###Blocks").
- Blocks on the schema are resized by the corner grip (same size on screen at any zoom);
  sizes are saved with positions in .karkas/layout/<view>.json ([x, y, w, h]); minimum
  120 × 56. The text scales with the block, long lines end with "…", ports that do not fit
  fold into "+N". Context menu: "Reset size"; background menu: "Auto layout (reset positions
  and sizes)"; settings: "Reset block sizes in this view".
- Headless checks: `--lang en|ru`, `--block-scale F`, `--settings`, `--drag x1,y1,x2,y2`
  (scripted mouse drag). Verified under Xvfb: both languages; scale 0.6 / 1 / 2; dragging a
  block far below the minimum stops at 120 × 56; enlarging unfolds the hidden ports; sizes
  survive a restart. kk_gui_tests: translations complete, settings round-trip and clamping.

## Stage 11 (first part) — import of existing projects (docs/adr/0009)
- Card directive `files` (leaf/entry/shared): a block over existing files that stay in place;
  E09; index roles by extension; drift checks, statuses, generation and agents aware of it.
- `karkas import [--dry-run|--apply] [--exclude DIR]`: deterministic cut of C/C++ code into
  groups (folders) and leaves (file pairs), fn ports from public functions, call/use links from
  calls and includes, purposes from header comments; proposes a @change for approval.
- `karkas init` in a folder with code keeps its C++ standard and suggests `karkas import`.
- GUI: call/use links between two blocks drawn as one line with a tooltip; at most 10 pin rows
  by default; an empty schema explains what to do.
- Verified on tests/fixtures/legacy (golden, clean schema and drift, approval through a change)
  and on a copy of a real 4.5k-line raylib game: 3 groups, 16 blocks, 120 links, 0 errors,
  0 warnings, 0 drift.
- Not done: agents inside imported blocks, clustering of big folders, `--describe` with a model,
  type-aware call matching.

## GUI addition — focus, open in place, address bar, projects (docs/adr/0010)
- Links colored by the block they start at (setting: by block / by link kind); hover focus
  dims the rest; Shift+click keeps links of several blocks lit.
- Shift+double-click opens a group in place with its whole subtree in a frame; the frame's
  top edge is a path bar that closes the group (layout_nested in kk_layout, tested).
- Pins flow left to right (inputs left, outputs right): call lines no longer cross blocks.
- Address bar like a file manager (click a part, or type a path).
- "Open project…" (Ctrl+O), recent projects, start without a project, open folders in the
  file manager; `karkas gui` outside a project opens the picker.
- Headless checks: --script with move/click/dclick (shift)/drag/wheel/type/key; verified by
  screenshots on a 4.5k-line imported game and wavkit.

## GUI addition — IDE layout and look (docs/adr/0010, addendum)
- Dark IDE theme, UI/heading/monospace fonts, ~35 vector icons rasterized into the font atlas.
- Activity bar + side bar (explorer with icons, status dots, filter, context menu; search over
  blocks, purposes and ports; schema changes with a colored diff and approve/reject; settings),
  editor tabs (schema with an icon toolbar; code files, read-only, monospace), inspector with a
  colored KSL card and a project overview, bottom panel with icon tabs and counts, status bar
  (drift counts, working agents, highlight, language switch).
- Blocks in their own colors (title band, tinted body, status dot, problems on the border);
  links start in the block's color; "All levels" uses the opened-in-place view (wheel zoom
  everywhere). Checked by screenshots in both languages.

## Addition — models per level and block, settings window, links, new project (docs/adr/0011)
- karkas.toml `[levels]` (tier per level of the block tree, 0 = the project) and `[blocks]`
  (tier per block and everything inside it); order block → level → [roles]/routing, for every
  agent on that block; the journal says what pinned the model. `[models.*] api_key_env` names
  the variable with the key of that API (several providers side by side, keys never in files).
- `core::set_config_value` edits karkas.toml one key at a time, keeping comments; tests.
- GUI: settings in a separate window (gear, Ctrl+,) with pages Appearance / Schema / Models and
  API / Help. Models page: APIs (add, edit, delete; key variable set or not, never the key),
  model of each level, blocks with their own model, role defaults. Schema toolbar: model chip
  for the level shown and the selected block; inspector: MODEL section.
- Links: right click on a line removes one of its links or all of them; Delete removes the
  selected line; one KSL preview for the whole removal.
- New project from the GUI (Karkas → New project…, Ctrl+N, or the Create tab of the project
  window): a new folder, or an existing folder (with code: the import is proposed at once).
  `code::init_project` is shared with `karkas init`; tests.
- `karkas approve api` also clears needs_review of blocks without an api/ (the entry).

## Addition — guide for coding agents, MCP write side (docs/adr/0012)
- templates/target/karkas_guide.md: the Karkas method for an agent; written into CLAUDE.md by
  `karkas init` / GUI New project, refreshed by `karkas gen guide` (managed block, the user's
  text is kept); MCP instructions point to it; tool kk_guide returns it.
- MCP tools kk_ksl_reference, kk_check, kk_propose_change (checked, stored as C-NNNN for the
  user to approve; never applied), kk_changes. `agents::check_diff`, `agents::propose_change`.
- Tests: guide block idempotence and user text kept, check_diff writes nothing, propose_change
  refuses broken or empty diffs, MCP tools and golden session; end-to-end `karkas mcp` on a copy
  of Flappy-Birds (proposal stored, broken one refused, stdout only protocol lines).

## Addition — models of karkas.toml for external agents (docs/adr/0013)
- `agents::model_for` / `route_block`: the model of a block as Karkas's own agents pick it
  ([blocks] → [levels] → [roles] / routing); the engine uses the same `route_block`.
- MCP tool kk_model_for(block?, role?): tier, model, reason, and the Claude Code subagent to
  delegate to (or "not a Claude model").
- `.claude/agents/karkas-<tier>.md` with `model:` from [models.<tier>] for every Anthropic tier:
  `karkas gen agents`, `karkas init`, GUI New project; the GUI rewrites them when an API is saved
  or removed. Hand-written files (no karkas:generated) are kept, stale generated ones removed.
- Guide section Models and MCP instructions: call kk_model_for, delegate to the subagent.
- Tests: resolution order and reasons, non-Claude and undefined tiers, unknown block/role, MCP
  tool wiring, subagent files (rewrite, keep, remove, names); end-to-end `karkas mcp` on a copy
  of wavkit with [levels]/[blocks].
- Not verified here: Claude Code actually delegating to the generated subagents (manual).

## Addition — benchmark method B with the Karkas method, subscription runs (docs/adr/0014)
- B: `karkas init` in S1, guide in CLAUDE.md (and --append-system-prompt-file under --bare),
  subagents of the tiers through --agents, MCP; self-approval counted as interventions.
- `karkas bench run --auth subscription`: claude.ai login (API key removed from the child's
  environment, `runner::RunOptions::unset_env`), user settings/skills/MCP left out; results carry
  `auth`, the report lists subscription runs apart (estimated costs). A: --strict-mcp-config.
- Tests: command lines of A/B × api/subscription; S3 run of method B with a fake `claude`
  (Karkas frame written, the API key never reaches it, results JSON); runner unset_env.

## Addition — fixes from the Blobby Volley 2 experiment (docs/adr/0015, 0016, amendments of 0013, 0014)
- Import of real code: the analyzer no longer aborts on class template specializations
  (`struct X<T, true>`); `karkas edit add-group | move | rename | purpose | remove` (op_move,
  op_set_purpose) to regroup an imported schema.
- Schema first in imported projects: a listed file that does not exist yet is W07, not E09.
- The project's own build (CMakeLists.txt not generated by Karkas): `approve api` freezes without
  generating a build; kk_run_tests / `karkas test` build it as it is and run `[test] command`
  (or ctest), output compressed; new keys `[project] cmake_args`, `[test] command`.
- Delegation: kk_view ends with the model line of the block; guide, MCP instructions and
  kk_model_for make handing over to karkas-<tier> plus a review against the card a rule; a guide
  section for imported projects.
- MCP: every tool call is logged with its time; logs flush on info (the server is ended by a
  signal, buffered lines were lost).
- Bench: stream-json saved per session, tokens per model and tool calls in results, a "tools and
  models" section in the report; agents' builds get KARKAS_DOCTEST_DIR (generated CMakeLists read
  it). Hidden tests (outside the repository): "feedback above 0.99 is clamped" — it catches the
  incomplete echo fix of the wavkit pilot's method B (11/12), which the old suite passed.
- Tests: W07 and a schema-first proposal, own-build test service (command ok / failing / not
  configured, CMakeLists never written), approve api with an own CMakeLists, op_move/op_set_purpose,
  model line in kk_view, stream-json parsing and report.

## Addition — cheaper Karkas sessions (docs/adr/0017)
- Measured on the Blobby logs: B paid +53% cached tokens on small fixes (heavy start ~50%, large
  kk_view/kk_check answers ~28%, orientation turns ~22%).
- Short guide in CLAUDE.md (path by task size) + full method in kk_guide; shorter MCP instructions.
- kk_locate / `karkas query locate`: ranked blocks with files and symbols for the words of a
  symptom (IDF, strict morphology, general synonyms, top-3 words decide).
- Views group repeated calls (L1 external, L2 callees/callers); kk_check summarises warnings.
- Delegation only for larger work; wait for subagents instead of polling.
- Tests: locate on wavkit and on imported code with symbols, kk_locate over MCP, issues_summary,
  card order of L2 neighbours kept (wavkit golden views unchanged).
- Experiments (outside the repository, karkas-bench/blobby2): bug hunt with 5 injected bugs and
  hidden tests — Sonnet: A 5/5 $0.28, B 5/5 $0.45; Haiku: A 5/5 $0.085, B 4/5 $0.086.

## Stage 10 — benchmarks (harness)
- bench/wavkit: task description spec.md (exact CLI and formats), five S2 extensions
  (s2_tasks.txt), five S3 bugs as exact-text patches (ring wraparound, double gain, no clip
  protection, echo feedback overflow, byte order), an example of hidden acceptance tests
  (hidden_tests.example/run.py: 11 S1 tests + 5 S2 tests; the reference passes all S1).
- `karkas bench run --scenario s1|s2|s3 --method A|B|C --model <m> --reps N`: fresh copy per
  run, unique marker at the start of every prompt, method A strips .ksl/.karkas, B adds the
  Karkas MCP server, C runs the Karkas pipeline (approvals counted as interventions);
  then build + hidden tests (path from `[bench] hidden_tests`); results in
  bench/results/<date>/<scenario>-<method>-<n>.json. Claude Code data from its JSON result
  (total_cost_usd, usage, modelUsage).
- `karkas bench report`: medians and min–max per scenario/method/model, cost by role for C
  (where the tokens go, 17.8); report.csv, report.txt, report.html.
- Tests: Claude Code JSON parsing, bug patches apply, report aggregation, and an end-to-end
  S3 run of method C with a scripted model (build + hidden tests).
- Not done: the measurements themselves (need an API key and money: ≥ 3 runs × 3 methods ×
  3 scenarios). The hidden tests must be copied outside the repository first (17.6).

## Not verified in this environment
- Real model calls: `karkas llm ping`, cache_read growth, "idea → working wavkit" (stage 7),
  the benchmark runs (stage 10) — they need ANTHROPIC_API_KEY and spend money.
- MCP Inspector and `claude mcp add karkas -- karkas mcp --project <dir>` (stage 3) — manual.
- The Graphviz engine — Graphviz is not installed (docs/adr/0002).
- karkas-gui was checked under Xvfb with screenshots; interactive use on Wayland is manual.
