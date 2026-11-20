# 0030 — An imported project's build file, failed full builds, a worker that blames the behaviour test

Status: accepted (extends ADR 0027–0029; found on Endless Sky, rounds 2 and 3)

## Context

Round 3 of the Endless Sky benchmark (pilot statistics: jumps, landings, trade income, kills per category, a new
panel) was chosen to favour Karkas: one feature across five groups and a new class. Karkas split it well (three
blocks, $0.22 against $0.97 of Claude Code alone) and still escalated:

- **A new source file never reached the build.** The worker of `ui.player.player_info` wrote
  `PilotStatisticsPanel.{h,cpp}` (its card gained them by an approved change) and wrote in fix "a human must add
  it to the build": the project's `source/CMakeLists.txt` belongs to no block, so no worker may edit it, and the
  worker prompt said so ("a new source file is added to it by a human"). The blocks' compile checks
  (`-fsyntax-only`) passed; the full build after the last block failed at the link.
- **A failed full build with no file in its errors escalated at once.** ADR 0028 sends the errors of a full
  build to the block whose file an error names; a linker error names none
  (`<artificial>:(.text+0xa39e): undefined reference to 'PilotStatisticsPanel::PilotStatisticsPanel(PlayerInfo&)'`),
  so the ticket went to the human right after its last block, with no run that could fix it.
- **A fix worker that blamed the behaviour test went up, not to the tester** (round 2, C3 run 5, bugs.txt 17):
  it found that the test's target could never be killed by the player and answered not_here naming the test's
  file; ADR 0029 sends only needs_tester to the behaviour tester.

- **A new block of an imported project had no files** (round 3, second run): the orchestrator proposed two new
  leaves (`campaign.player.pilot_statistics`, `ui.player.pilot_statistics`) without `files`; the change was approved,
  the worker of the first found only a block.ksl ("no api/, no impl/") and answered needs_info, the next blocks
  lacked its type, and the ticket escalated.

## Decision

1. **Karkas adds a block's new source file to the project's build itself** (`agents/build_files`, deterministic).
   After a worker of an imported block reports done, every code file of its card (and of its report) that exists
   and that no `CMakeLists.txt` of the project names is added to the `CMakeLists.txt` that names another code file
   of the same folder: before the first file of that folder that sorts after it, else after the last one; on a
   line of its own when the list has one file per line (Endless Sky), inline otherwise (`add_executable(game
   src/a.cpp src/b.cpp)`); a header only where that list names headers of the folder. Paths are written relative
   to the list's folder. A project that globs its sources or uses another build system is left alone. The ticket
   notes it ("Karkas added the new file(s) of X to the project's build: source/CMakeLists.txt: Y.cpp, Y.h") and the
   worker prompt and build hint say Karkas does it.
2. **A linker error names the block of its symbol.** `undefined reference to`, `multiple definition of`, lld's
   `undefined symbol:` and `vtable for X` give the names of the symbol (`A::B::f(int)` -> A, B, f); the first one
   that is the stem of a code file of a card (case-insensitive: `World` -> `src/game/world.cpp`) names the owner,
   whose worker gets the errors as its task (ADR 0028's repair, twice at most). The schema is read again for it:
   the worker's card may have gained the file in its run.
3. **A full build no error of which names a block goes to the orchestrator once**, with the build's errors and the
   blocks done in the run free to be sent again; the block it dispatches gets the errors as its task. The second
   such failure (or the third repair) still escalates.
4. **A fix worker's not_here or needs_info that names the file of the ticket's behaviour test is a needs_tester**:
   the behaviour tester corrects the test with the worker's finding (ADR 0029's path). The fix task of a failing
   behaviour test now says to report needs_tester when the test itself is wrong.

5. **A new block with code in an imported project gets its files named** (`name_new_block_files`, in
   `propose_change`): the folder and extensions of its siblings' code files (else of the blocks it calls or uses,
   else of all imported blocks), the name its first `note types:` type or its last id segment in CamelCase, lower
   case where the siblings' files are (`game.hud` -> `src/game/hud.cpp src/game/hpp/hud.hpp`;
   `ui.pilot_statistics` with `note types: PilotStatisticsPanel` -> `src/ui/PilotStatisticsPanel.{cpp,h}`); a
   shared block gets a header only, a block with files keeps them, a name another block owns leaves the proposal as
   it is. A project of Karkas's own (no imported block) is unchanged. The worker prompt says that a file of its
   card that does not exist yet is its to create; decision 1 then adds it to the build.

Model choice stays configuration: a block scored 2 (difficulty M, more than 300 lines) goes to tier cheap with the
default `[routing] cheap_max`; a project where a cheap model keeps writing plausible but wrong code (Endless Sky:
`event.Actor()->IsYours()` in 5 of 6 Haiku runs) lowers `cheap_max`.

## Consequences

- A feature with a new class in an imported CMake project builds without a human; its behaviour test can run.
- Karkas now writes one file outside every card (the project's build list), only to add entries it can name.
- Tests: `build files: ...` (one-per-line and inline lists, headers, globs, linker names) and the imported tickets
  "a new source file of a block reaches the project's build by Karkas", "a linker error goes to the block of its
  symbol", "a full build no error of which names a block goes to the orchestrator", "a fix worker's not_here that
  names the behaviour test goes to the behaviour tester", "imported change: a new block with code and no files gets
  them as the project names its files", "imported change: a project of Karkas's own keeps a new block without files".
