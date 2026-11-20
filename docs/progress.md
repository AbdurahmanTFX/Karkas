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

## Addition — briefs and reviews for delegation, batch edits (docs/adr/0018)
- kk_brief / `karkas brief`: the deterministic part of a subagent's brief plus [fill] lines (task,
  spec with concrete numbers, done-when) that the delegating model must complete.
- kk_review / `karkas review`: guarantees checklist, changes outside the block flagged, the diff of
  the block's files, its issues and test status.
- `karkas edit batch`: a script of schema edits in one process.
- Guide: never hand over a brief with a [fill] left; accept only after kk_review.
- Tests: brief of a wavkit leaf (files, guarantees, neighbour api, tests), review in a git copy
  (inside and outside changes, diff), edit scripts (quotes, failures stop with the line).

## Addition — stay on the task (docs/adr/0018, amendment)
- kk_note_bug: a bug noticed on the way becomes an open ticket (origin agent) and is named in the
  report, not fixed. Rule in both guides, MCP instructions, subagent prompt and kk_brief.
- Bug hunt rerun of B on the optimised Karkas (before this rule): Sonnet 5/5 for $0.26 (was $0.45;
  A $0.28), Haiku 5/5 for $0.057 (was 4/5 for $0.086; A $0.085). In both runs the replay session
  also fixed the network bug, so the network session was stopped.

## Addition — stopping agents, clearing panels, orchestrator chat (docs/adr/0019)
- Stopping: process flag (`core::request_cancel`, SIGINT/SIGTERM in the agent commands; a second
  signal quits at once) and per-run stop files (`metrics::request_stop`); the loop ends with
  `cancelled`, curl aborts the request in flight, back-off gives up; no retry or escalation, the
  ticket gets a note and is `open` again, exit 130. Active markers carry the pid.
- GUI agents tab: Stop (this agent), Stop command (SIGINT to its process), Kill after 5 s.
- Bottom panels: × per row and "Clear list" (tickets, runs/costs, test results) with Restore;
  display only (`.karkas/cleared.json`), journals/tickets/karkas.log untouched; newer activity
  shows again.
- Orchestrator chat (Ctrl+Shift+O): messages run `karkas task` / `karkas debug` / `/<agent command>`
  as a child process; progress streams in; result with next steps (ticket, continue, approve
  C-NNNN); Stop / Kill; history in `.karkas/chat.jsonl`.
- Tests: cancel scopes, retries, stop files, loop and engine (stopped worker, SIGINT during a
  navigator), cleared rows, chat with a fake `karkas`. Checked by hand: SIGINT during a hanging
  model request (< 0.1 s, exit 130), GUI under Xvfb (chat task, Stop, clearing).

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

## Addition — contract-change requests without an applicable KSL diff
- A worker may ask for a contract change while sending a unified diff of `api/` (or nothing)
  instead of a KSL card diff. `parse_diff` used to fail silently, the descent created an empty
  @change, approval applied it with no effect and the next run repeated the same descent.
- The `report` tool refuses a `proposal` that is not a KSL diff (not terminal), so the model
  corrects it in the same run. The descent turns a proposal into a @change only through
  `propose_change` (changes a card, passes the schema checks); otherwise the ticket is escalated
  with the worker's `cause`, `fix` and the rejection reason, and no @change is created.
  `LeafOutcome::proposal_error` keeps the parse error. The worker prompt spells out the card-diff
  format and says to leave `proposal` empty when the card is right but `api/` is stale.
- Tests: refused code diff corrected in the same run, missing proposal, a diff that changes no
  card; the happy path (valid KSL diff → pending @change) is unchanged.

## Addition — invalid UTF-8 never aborts a run
- `shorten`/`clip` and the read_file cap cut text at a byte count and could split a code point;
  the broken string reached `nlohmann::json::dump` in the LLM request (the orchestrator's
  "recent tickets" context) and aborted `karkas ticket run` (`type_error.316`).
- `core::truncate_utf8` cuts on a code-point boundary; `shorten`, `clip` and the read_file cap
  use it. LLM request bodies (Anthropic, OpenAI-compatible) and run journal lines are serialized
  with `error_handler_t::replace`: invalid UTF-8 from any source (a file in another encoding, a
  model's output) becomes U+FFFD instead of an exception.
- Tests: truncate_utf8 with 1–4-byte code points against a UTF-8 validator; both providers
  serialize a request with a broken byte.

## Not verified in this environment
- Real model calls: `karkas llm ping`, cache_read growth, "idea → working wavkit" (stage 7),
  the benchmark runs (stage 10) — they need ANTHROPIC_API_KEY and spend money.
- MCP Inspector and `claude mcp add karkas -- karkas mcp --project <dir>` (stage 3) — manual.
- The Graphviz engine — Graphviz is not installed (docs/adr/0002).
- karkas-gui was checked under Xvfb with screenshots; interactive use on Wayland is manual.

## Addition — report validates a proposal like propose_change
- Seen on T-0007 (flappy-karkas): a worker sent `~ @block x` then `~ purpose ...`; `report` only parsed it,
  the descent rejected it later ("'~' selects a record"), no @change was created and the ticket escalated
  after a full run. `check_proposal` (changes.cpp) is the dry run of `propose_change`; `report` now refuses
  any proposal that would not become a @change (the model corrects it in the same run) with the line-change
  syntax (`- old` then `+ new`).
- worker.txt: states the diff syntax, allows reporting needs_contract_change without build()/test() when the
  contract change is already clear; ksl_change_example.txt shows a line change.

## Addition — agents see the outcomes of earlier descents; no circles (docs/adr/0020)
- Found by a DeepSeek-only test on flappy-karkas (report: flappy-karkas/karkas-deepseek-test-2026-11-12).
- Path views: own decisions keep suspects and fold repeats (`repeats=N`); root and group views end
  with the earlier descents (`tried ...` with the cause) and the last result.
- Descent: answered blocks are not sent the ticket again in one run; needs_contract_change without a
  card diff goes up to the orchestrator (new tool `escalate`); feature leaves return to the
  orchestrator until it closes; new worker status `needs_tester` hands a contract-test flaw to the
  tester; the orchestrator's proposal is checked like a worker's.
- Re-runs: results carry `state`; `ticket run` refuses (no model started) while a change waits or when
  nothing changed since a blocking result; `--force`; `ticket close`.
- api/ in tickets: values of a frozen api may change (re-frozen); an api unfrozen by an approved change
  is updated by the worker; the architect sees the current api/ and keeps its style.
- Models: `[routing] escalate`, `[effort]` per role, `reasoning_effort` and `reasoning_content` in the
  OpenAI-compatible provider, tester tier via tier_for, probes tool only with data.
- Tests: path views (core), api_shape and api access (sandbox), 13 engine scenarios (feature over two
  leaves, needs_tester, no re-dispatch after not_here, blocked re-run, pending change, escalation
  chain, unfrozen and frozen api, orchestrator proposal check, ask cut, probes/journal, effort),
  provider reasoning/effort, config parsing. 259/259 green.

## Addition — GUI: panels over the schema, live feed, ghost of a change, faster canvas (docs/adr/0021)

- Layout: the schema covers the work area; the side bar, inspector and bottom panel lie over it
  and are resized at their inner border; no docking; the chat floats on top; fit to view uses the
  free part. Windows menu hides/shows every panel and bottom tab.
- Chat: browser-like selection of every text (drag over messages, double/triple click, Shift,
  Ctrl+A/C, menu); Live tab shows every step of any agent run (also from a terminal); Auto mode
  (`[agents] auto_approve`); proposed changes: show on schema (ghost), edit, approve, reject.
- Changes panel: rows no longer move under the mouse; hide works; selecting a change opens its
  level and draws new/removed blocks, ports and links.
- Canvas: lines attach on the facing side, socket shape per link kind, direction chevrons;
  per-model caches + level of detail: "All levels" of blobby 52–62 ms → 17–19 ms per frame (debug).
- Cursors from the desktop theme (theme names resolved by Name=); symbol glyph ranges; times and
  costs in tickets, runs and agents; tickets show titles first.
- Tests: 272/272 (`ctest --preset dev`; `index/perf` is timing-sensitive under `-j8`, passes alone).

## Addition — lines can be bent by hand (docs/adr/0022)

- Hover a line: a "+" in the middle of each piece (drag it to bend the line there) and its bend
  points (drag to move, double click to remove); line menu "Straighten the line", background menu
  "Straighten every line of this level". Bends follow the blocks (kept relative to the two blocks'
  centers) and are saved in `.karkas/bends.json`; a bent line leaves its blocks on the side facing
  its bends and passes under blocks. "All levels" of blobby stays ~19 ms per frame (debug).

## Addition — zoomed out, blocks are cards with their names (docs/adr/0023)

- Below ~8 px of text on screen a block shows only its name, large and centered, on its color;
  zooming in brings back icon, status, purpose and ports. Pins keep their places (lines do not
  move); sharp large names from a 28 px raster of the medium font. Tests 272/272; "All levels" of
  blobby ~18 ms per frame (debug).

## Addition — agents through MCP: the same agents, an MCP client as their model (docs/adr/0024)

- `[agents] mode = "api" | "mcp"` (`karkas mode [api|mcp] [--fit]`, the GUI switch in the chat,
  the status bar and Settings → Models). In mode mcp the engine's turns go to `.karkas/mcp/turns/`
  and Claude Code answers them through kk_agent_next / kk_agent_act; kk_task / kk_debug /
  kk_ticket_run start tickets. Same prompts, tools, sandbox, checks, escalation, journals.
- Per-mode model maps: [levels]/[roles]/[blocks]/escalate for api, [mcp.*] for mcp (Claude tiers
  = Claude Code subagents); `--fit` / "Fit to the API models" sets the MCP maps from the API ones
  by strength. flappy-karkas: mode api, MCP maps fitted (0 strong, 1 cheap, 2 strong).
- Tests 279/279; `karkas mcp` checked over stdio with a scripted client.

## Addition — agents see why they fail, a client never waits on itself (docs/adr/0025)

- From flappy-karkas T-0013 (mode mcp) and the test-free-AI logs (OpenCode + DeepSeek, $0.48):
  `.gitkeep` counted as a C++ source broke the configure of the whole project after C-0004
  (`add_executable` with no sources); CMake errors lost their message; sandbox refusals named
  nothing readable (42 of 50 integration-tester steps were errors); the entry block had no test
  program (T-0003: 75 steps of probes); `karkas approve` in the client's own shell waited on
  that client.
- Fixed: exact extensions and `if()` guards in the generated CMake, entry unit tests (impl/
  without main.cpp); CMake message lines in the build summary; sandbox and tool messages that
  list what is readable / what is there; build hints for errors in others' files and for "tests
  none found"; doctest named in the worker prompt; kk_implement and the "run it in the
  background" notice in mode mcp; kk_agent_next(run, after) gives only what is new; stopped
  results carry `state`; D04 E vs W wording; the tester adds contract tests for a feature's new
  invariants of a leaf; two proposed changes stay two lines of the path view.
- Tests 287/287 (`index/perf` timing-sensitive under `-j8`, passes alone).

## Addition — flappy-karkas T-0014 through MCP: five more fixes (docs/adr/0025, addendum)

- Feature "the pace rises with the score, the HUD shows ×1.10" over config, rules.round, view.hud,
  served through `karkas mcp` (orchestrator, workers and testers of tier strong by this session, cheap
  by karkas-cheap subagents): done, `kk_check` 0/0, all tests green, contract tests for the four new
  invariants, the contract autopilot made fit for the faster world.
- Fixed on the way: standalone tests with their own main() are programs of their own; the api note
  allows new constants; `needs_tester` in a feature goes back to the orchestrator and keeps the
  worker's files; `--force` reopens a closed ticket (kk_ticket_run says so first); read_file
  offset/limit keeps indentation.
- Cost (client model of T-0013): T-0013 $1.15–1.40, T-0014 $1.95–2.42, of which ~$0.5 came from the
  two engine defects and one extra card change; 86 % of the reading were whole turns of new runs.
- Tests 289/289.

## Addition — fewer and cheaper runs per ticket (docs/adr/0026)

- Plans: the orchestrator of a feature gives `next` with dispatch / propose_change; the blocks run one
  after another and the ticket closes after the last (back to the orchestrator only on a deviation or a
  block of the approved change nobody sent); a plan waiting for a human goes on after the approval.
- Auto in the worker's run: a worker's card diff is approved by its report and the worker goes on.
- Mechanical edits on approval: `+ const <name> <type> <value> // note` lines are written into api/;
  new fn are declared when the header's style and the types allow it.
- Optional `[agents] review_changes`: the leaves of an orchestrator's change say whether their card is
  enough before it is approved.
- Cheaper turns: role prompts without the block name (one cached prefix per role), the KSL reference as
  a tool, neighbor headers as the declarations the block uses, unit tests as a list, the last result per
  block; in mode mcp `kk_agent_next(seen=[...])` leaves out the system prompt, tools and unchanged
  context of a role the client served.
- Routing: [levels] pick the deciding roles only; leaf work by [roles] and task size
  (`[routing] levels_for = "all"` for the old pinning).
- Measurement: MCP traffic per turn (`.karkas/mcp/traffic`) → estimated tokens and cost in the journals
  and the ticket; `karkas stats [ticket]`.
- Test infrastructure: a test that runs no case fails (';' in TEST_CASE names had hidden nine tests).
- Measured on flappy T-0015 (same scope as T-0014): ≈ $0.31–0.34 for the client (T-0014: $1.95–2.42;
  T-0013 done directly: $0.26–0.40); 8 runs, 2 orchestrator decisions, all leaf work on the cheap tier.
  A ticket's tester escalates its tier on failure; act-delivered turns are counted in the traffic.
- Tests 294 (`index/perf` timing-sensitive under `-j8`, passes alone).

## Addition — agents work inside imported blocks (docs/adr/0027)

- From Endless Sky (imported, ~500 files): T-0001 "low-fuel warning with a preference" through
  kk_task in mode mcp was escalated before any worker ran ("agents do not edit imported blocks yet").
- Sandbox of an imported block: paths from the project root; reads its files, the headers of linked
  blocks and of its users, unowned non-code files (not hidden, not build/, .karkas/, blocks/); writes
  only the files of its card. grep over "." searches its files and those headers; list_dir lists only
  what is readable. An approved card change rebuilds the sandbox in the same run.
- Headers: `ApiAccess::Imported` — the public part is the contract (`header_contract_kept`: textual
  scanner with scopes and access labels; private members, inline bodies, initial values, comments,
  new includes and public constants are free). On the headers of Endless Sky: a public method added
  is caught in 192/192 headers, private members pass in 166/166, the header edits of methods A and B
  pass. A frozen block is re-frozen after such an edit.
- Checks: `[project] build_command` (the project's own build; Karkas configures nothing), compiler
  warnings of the worker's own files after build()/test(); the tester never runs on imported blocks,
  their new invariants are reported as checked by the project's own tests.
- Data files: `files` may list non-code files (owned like code, skipped by the index and analyzer).
- Worker: prompt `worker_imported.txt`; context with the listed files (small whole, big by size) and
  the declarations of linked headers the block uses — big classes as head, labels and the used public
  members (the most called blocks first).
- Found on the way, not fixed (bugs.txt 7, 8, 9): ~100 s index updates on Endless Sky; a misleading
  "strongest tier failed" note for a worker that never ran; spawning the engine after a rebuild.
- Endless Sky T-0001 again (`ticket run --force`, mode mcp, every turn served by one Opus session):
  closed done. The orchestrator planned data.settings.preferences → flight.engine →
  ui.menus.preferences in one decision; three workers did their blocks in 3, 3 and 5 steps, each
  done at the first attempt; the settings screen's worker took data/_ui/tooltips.txt into its card
  (C-0001, approved in its run) and wrote the tooltip. 6 files, +28 lines; the changed sources
  recompile without warnings (-Wall); `karkas check` 0 errors (the 46 warnings were there before).
  Turn traffic of the run ≈ 296 K characters (the three worker turns ~83–90 K each).
- Tests 305.

## Addition — faster and cheaper tickets on a big imported project (docs/adr/0028)

- Measured on Endless Sky round 2 (C: 24 min, $0.66; the models worked ~3 min): index updates ~10 min,
  builds ~9 min, the coordinating Opus session $0.63 (61% cache writes of the orchestrator's big views).
- Index: links computed once per update (allowed_api_blocks, block_test_hash, ordered_children), D09 by a
  name → owners map, includes tried only against roots that hold files: 90 s → 5 s per update.
- Orchestrator: the 8 blocks `locate` finds for the ticket's words in its first decision; compact views of
  big groups (grouped internal calls, the 20 heaviest external pairs) and of neighbors in L2 views.
- Workers of imported blocks: neighbor headers with their own 20 KB budget, only with used declarations;
  build() and the check after the report compile the card's sources by compile_commands.json with
  -fsyntax-only; the full build and tests run once after the last block; a failing full build goes back
  to the block whose file the error names (twice at most).
- Coordinator: the MCP instructions say to wait with kk_agent_next(wait_s=300) and not to look up
  kk_changes under auto_approve; `[mcp.roles] orchestrator = "medium"` with the session on Sonnet.
- Endless Sky round 2 again (C2: Sonnet coordinator and orchestrator, Haiku workers): closed done in
  10.1 min and $0.23 (Sonnet $0.21, Haiku $0.02) against C 23.9 min / $0.66, A 13.4 min / $0.41, B 14.8 min
  / $0.63. Orchestrator: 22 s and 12.6K input tokens (was ~3.5 min, 53.6K); index updates 3-6 s; workers
  53 s and 45 s; one full build ~6 min. The Sonnet orchestrator wrote the new fn into a note only, so the
  panel's worker proposed the missing card lines itself (C-0002, approved in its run).
- The same on Opus (C2-opus: session and orchestrator strong): 8.4 min and $0.54 (Opus $0.52, Haiku
  $0.02); the orchestrator decided in 17 s on 13.4K tokens with the complete change (fn, call, notes),
  workers 54 s and 30 s, full build ~4.3 min. The coordinating session's own context, mostly 1-hour cache
  writes, is now the price; the medium tier halves it.
- Checked in the running game (a test plugin in the game's test mode): C2 and C2-opus count, save, load and
  show; B's round-2 code (method B, Karkas as the map) compiles but never counts (it reads event.Actor(),
  which the game leaves empty for destroyed ships).
- Found: kk_agent_next(list) waits out wait_s after the ticket ended; coordinators run `karkas approve
  api` from the ticket's note (bugs.txt 10, 11).
- Tests 308.

## Addition — cheaper, faster and surer tickets (docs/adr/0029)

- Endless Sky after ADR 0028: the coordinating Claude Code session was 90-96% of a ticket's money, the release
  build with LTO most of its time, idle waiting and wrap-up after the ticket ~2 min; method B's feature
  compiled and never worked, and nothing in Karkas would have caught it.
- `[agents] launch = "claude"` (RunLauncher): in mode mcp the engine serves its runs itself, one headless
  Claude Code per run on the claude.ai login (no API key in its environment), the model of the run's tier,
  a three-sentence system prompt, only kk_agent_next / kk_agent_act (`karkas mcp --only-agent`), started
  outside the project; costs in .karkas/mcp/launch/costs.jsonl; `karkas bench --method C --auth subscription`.
- The end of a ticket: kk_agent_next(list) returns when the tickets have ended, with their result; auto_approve
  freezes the api it approved; the instructions say only the report comes after; self_executable() survives a
  rebuild (bugs.txt 9-11 fixed).
- Behaviour tests of imported features: `[test] dir`, `guide`, `command`; a tester writes one test in the
  project's framework after the last block, KK_TEST_FILES names it for the test command, a failure goes back
  to the orchestrator. Endless Sky: tests/karkas/run.sh runs a test plugin headlessly and checks
  `# expect-save` lines of the pilot file.
- Orchestrator: a fn named only in a note sets the change aside (no model); after a decision that came back
  the next one is a tier higher; review_changes on by itself below the strongest tier.
- Views: a card with more than 24 call lines shows one line (PlayerInfo's L2 view 29.7K -> 13.2K characters);
  an imported worker gets the linked declarations of the functions its task names; D04 checks return types.
- Index: tickets, changes, probes and run journals refresh only their tables; `karkas check` reads the
  issues of the index (Endless Sky 4.9 s -> 0.4 s, the same 46 warnings); facts keyed by the analyzer's
  build; drift.col; SQLite waits 30 s; logging calls are no D09; kDbSchemaVersion 2 (its rebuild path fixed).
- `[project] verify_command`: a quicker build checks the agents' work (a debug build; the game's debug
  binary runs its tests ~8x slower).
- Tests: `slow` label for the four reference builds, `ctest --preset dev-fast`; ccache for their builds.
- Measured on Endless Sky, method C3 (orchestrator Opus, workers Haiku, behaviour tester Sonnet), four runs,
  each fixing what the one before showed: the behaviour test caught method B's `event.Actor()` bug every time
  a Haiku worker made it; a failing test may send a done block again, a tier up (medium, then strong), with
  the orchestrator's cause in why; run 3 closed done and works in the game (1 -> 2, $1.15); run 4 works but
  escalated: the tester's test waited for a kill its scenario cannot give, and the fix worker's needs_tester
  went to a contract tester (now to the behaviour tester, bugs.txt 16). The subscription's usage limit no
  longer fails a run or raises its tier: the launcher pauses, the turns wait (bugs.txt 15). Method B on this
  Karkas: 2.3 min without compiling (was 8.6), the same broken counter.
- Tests: 329 (dev-fast 325 in 25 s, slow 4 in 227 s).

## ADR 0030 — an imported project's build file, failed full builds (2026-11-20)

- A block's new code file reaches the imported project's own build: after the worker's done Karkas adds it to the
  CMakeLists.txt that names the files of its folder (`agents/build_files`: sorted, one per line or inline, headers
  only where listed, globs left alone). Endless Sky's round 3 had escalated on the link of a new panel class.
- A linker error names the block of its symbol (`undefined reference to 'A::f'` -> the card with A.cpp/A.h); a
  full build no error of which names a block goes to the orchestrator once with its errors, not to the human.
- A fix worker's not_here that names the behaviour test's file goes to the behaviour tester (bugs.txt 17); the
  model of a launched run whose safeguards refuse it moves a tier up (bugs.txt 18).
- A new block with code in an imported project gets its files named as the project names its files
  (bugs.txt 20).
- Tests: 341.

## ADR 0031 — the sessions' own cost, a done that changed something (2026-11-20)

- A run served by a launched Claude Code session costs what the session reported (costs.jsonl): runs index
  (schema 3), `karkas cost`, the GUI and the ticket's cost line; round 3 run 3 said $0.19, its sessions $1.08.
- The estimate for other MCP clients models the prompt cache (1-hour writes, reads, thinking): 0.81-1.13 of the
  real cost on four runs (was 0.2-0.3); `karkas cost` prices 1-hour cache writes at 2x; Sonnet's cache read $0.10.
- A worker's done with no file of its block changed goes to the orchestrator (bugs.txt 14); a planned block's
  not_here/needs_info too, not up the tree (bugs.txt 13); a session reads only its own part of the run log.

## GUI addition — tiles, floating tiles, tiles as windows of the desktop (docs/adr/0032)

- The panels are tiles of an ImGui dockspace over the schema (its empty central part lets the mouse
  through; the schema keeps the whole work area): side bar, inspector, each bottom tab and the chat.
  A tile is docked (dragged by its tab to any edge, tabbed with others), floating inside the window,
  or a window of the desktop with the window manager's decoration and a task bar entry; chosen by
  drag, the tab's context menu, the header button or Windows → Tiles and windows (also Reset).
  The chat docks as its own tile under the inspector.
- Wayland (the user's Hyprland): virtual positions for the windows of the desktop, one monitor per
  window (popups stay inside their window), floating windows kept inside the main window
  (viewports.cpp). X11 keeps ImGui's behaviour. App id "karkas" for desktop rules.
- Layout kept in ~/.config/karkas/gui_layout.ini, tiles sent out in gui.json; `--layout FILE` for
  checks; `KARKAS_GUI_LIST_WINDOWS=1`, `KARKAS_GUI_VIRTUAL_WINDOWS=1`, `KARKAS_GUI_WINDOWS=0`.
- Checked under Xvfb (`env -u WAYLAND_DISPLAY`): default layout, chat docked, chat as a window of the
  desktop (its screen captured), drag clamp, layout restored after a restart. Tests 341.

## GUI addition — editing cards, guarantees and tickets in place (docs/adr/0033)

- Inspector: "Edit" turns the card into KSL text; "Check and save" → `agents::op_replace_card` (parse,
  same id, the lines it removes and adds) → the KSL diff preview → checks → undo. "Edit guarantees":
  the inv lines one by one (`agents::op_set_invariants`), for leaves and shared blocks.
- Tickets tile: "Edit" the whole ticket; `core::TicketStore::edit_by_human` parses it, keeps its id,
  refuses while an agent holds its lock; the append-only rule still binds the agents.
- Tests 344; checked under Xvfb (card edited and applied, ticket fact added and saved).

## Addition — chat scope: whole project, one block, one ticket (docs/adr/0034)

- Ticket field `scope`; `karkas task|debug --block <id>`: the first run starts at the block (no
  orchestrator decision), the orchestrator later dispatches and plans only inside the scope.
- `karkas ticket run <id> --note "..."`: the human's message becomes a fact `human: ...`, the ticket
  runs again (a closed one reopens).
- GUI chat: "Request:" whole project / block / ticket, following the last selection, with a line
  saying what happens; inspector "Chat" button, ticket menu "Write about this ticket in the chat".
- Tests 349 (scope_test.cpp, chat commands per mode).
