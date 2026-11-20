# 0027 — Agents work inside imported blocks

Status: accepted (deviation from spec 9.5 and the "for now" of ADR 0009; extends ADR 0016)

## Context

ADR 0009 let imported blocks name their existing files (`files`) and left agents out of them "for
now": their sandbox is the block folder with api/ impl/. ADR 0026 closed with the consequence: a
benchmark of the agents on a big codebase needs agents on imported code, and every big project here
is imported.

Endless Sky (~500 files, imported, its own CMake presets) showed it plainly. The feature T-0001 ("a
low-fuel warning with a preference that turns it off") went through `kk_task` in mode mcp: the
orchestrator picked flight.engine with a plan over data.settings and ui.menus, and the ticket was
escalated before any worker ran — "block is imported code (files): agents do not edit imported blocks
yet". The same feature done by Claude Code directly (method A) and with Karkas as a map (method B)
touched seven files: Engine.cpp/.h, Preferences.cpp/.h, PreferencesPanel.cpp and two data files.

Four things stood in the way:

1. The sandbox has write roots only for impl/ and tests/unit/; an imported block has neither.
2. The headers of an imported block are its api. A worker gets no access to api/ unless it is frozen
   (values only) or unfrozen by a change, and the values-only rule refuses any new declaration: a
   private field in Engine.h is a "contract change" although no user of the class can see it.
3. The worker's checks are contract tests by the tester and Karkas's own CMake. An imported block
   has no contract tests, and for a project with its own build `build_and_test` still configured
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug` — on Endless Sky a second, full Debug build
   next to the project's own preset tree, far beyond the build timeout.
4. Data files (Endless Sky keeps messages and tooltips in data/*.txt) belong to no block.

## Decision

### The sandbox of an imported block

- Paths are relative to the project root, as the card lists them and as includes and compiler errors
  name them (`source/Engine.cpp`). The block folder (its block.ksl) stays readable under its path.
- Readable: the block's listed files; the headers of the blocks it is linked to, and of the blocks
  that use it (imported neighbors by their paths, native ones as `<block>:api/<file>`); project files
  that are not code, not hidden, not in the build folder, `.karkas/` or the blocks folder, and that no
  card lists (data and config the worker may need to find; see below).
- Writable: the block's listed files only — sources always, headers by the rule below. A listed file
  that does not exist yet (W07) may be created. New files and files of other blocks need a card change.
- `grep` over "." searches the block's files and the neighbor headers; over a named folder, its
  readable files. `list_dir` shows only what can be read.

### Headers: the public part is the contract

A new access mode `ApiAccess::Imported` (the default of an imported block's worker, frozen or not):
the declarations other code can see stay as they are. Free are: everything in the private sections of
classes (fields, helper methods, nested types), bodies of inline functions, initial values, comments,
new `#include` lines, and new public constants (`static const`, `constexpr`, `const` without a
parameter list). A removed or changed public or protected declaration, a new public function, type or
field is a contract change: the worker reports needs_contract_change with a card diff (`+ fn ...`),
which Auto approves in the run; the header is then unfrozen (`Full`) for the rest of the run. A change
of this ticket already approved for the block gives `Full` from the start.

The check is deterministic (`header_contract_kept`): comments and string literals are skipped, the
header is cut into declarations by `;`, `{`, `}` with the scope of each (namespace, class and its
current access); bodies of functions and initializers are opaque; initial values of variables become
a placeholder. Every visible declaration of the old header must stay; a new visible one must be a
constant or an `#include`. When only free parts changed and the block was frozen, its frozen hash
follows the file (as for tuned constant values), so D05 stays quiet.

### Checks: the project's own build and tests

- New key `[project] build_command` (for example `cmake --build --preset linux-release`). With an own
  build and this key, the build of `build_and_test` is that command (from the project root, with the
  build timeout); Karkas configures nothing. Without it the old configure + build stays.
- `[test] command` (or ctest) runs after it as before (ADR 0016).
- The build reports warnings too: a worker sees the warnings in the files it may change after
  `build()` / `test()` ("compile without new warnings" is a common requirement of imported code).
- The tester never runs on an imported block (no Karkas contract tests there). New invariants of an
  imported block in a feature are reported as checked by the project's own tests only.

### Data files

`files` may list files that are not code (`files data/_ui/messages.txt`). They belong to the block
like its code (E09: one owner; W07 until they exist), the index and the C++ analyzer skip them, and the
block's worker may change them. A worker that needs an unowned data file reads it (readable, see
above) and proposes `~ @block <its id>` / `+ files <path>`; with Auto it goes on in the same run: the
sandbox is rebuilt from the approved card.

### The worker

A separate prompt (`worker_imported.txt`) tells the paths, the header rule, the data files and that
the project's own build and tests check the work. Its context has the listed files (small ones whole,
big ones by size: read them by `offset`/`limit` or `grep`), the neighbor declarations the block uses
(as for native leaves, ADR 0026) and no tests/ sections.

## Consequences

- A ticket over imported blocks runs end to end in mode api and mcp: orchestrator, navigators, workers
  with the project's own build as the check.
- Spec 9.5 changes for imported blocks: writing is the listed files, not impl/ and tests/unit/.
- New public functions of an imported class need a card line, as for native blocks; private changes do
  not. The analysis of headers is textual: a macro that hides a declaration without `;` (Qt's
  `Q_OBJECT`) is one declaration with the next one.
- Contract tests of imported blocks remain the project's business (ADR 0016).

## Measured

Endless Sky T-0001 run again after this change (mode mcp, `[project] build_command = "cmake --build
--preset linux-release"`): the ticket closed done. One orchestrator decision planned three leaves
(data.settings.preferences → flight.engine → ui.menus.preferences); each worker was done at its first
attempt (3, 3, 5 steps). The header edits passed the rule (a public `static const std::string`, a
private `bool`); the settings screen's worker read the unowned data/_ui/tooltips.txt, took it into its
card with `+ files` (C-0001, approved in its run) and wrote the tooltip. Result: 6 files, +28 lines —
the scope of methods A and B; no new compiler warnings, `karkas check` 0 errors.

On the headers of Endless Sky the rule caught a public method added to 192 of 192 headers and let
private members through in 166 of 166; the header edits of methods A and B pass it.

Still slow on a project of this size: an index update takes ~100 s (bugs.txt 7), paid by the ticket
process at its start and at each approval.
