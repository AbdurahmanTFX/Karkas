# 0037 — Build and run: a green triangle for the project's program

Status: accepted (amends 14.4 panels, appendix G CLI, appendix E karkas.toml)

## Context

The user asked (2026-11-21) for a build-and-run button — a green triangle, as in an IDE — that runs the
resulting project. Karkas built and tested the target project (`karkas test`, the agents' build and test
tools) but never ran its program: the human had to leave the GUI for a terminal.

## Decision

`karkas run [--no-build] [--show] [-- args...]` (apps/cli/cmd_run.cpp), for the human only (the chat
does not allow it, no MCP tool or agent tool runs it):
- The program (`agents::run_target`): `[project] run_command`, run by `sh -c` from the project root
  (arguments follow it as `"$@"`); else the program of the entry block that Karkas's generated CMake
  builds, `<build_dir>/<project name>` (5.5). A project with its own build (its own CMakeLists.txt or a
  `build_command`), an imported entry block, an entry without code: Karkas does not know the program and
  says so, listing the programs it finds in the build folder (`agents::program_candidates`: executable
  ELF files, newest first, without libraries, CMake's own programs and the generated `*_tests` /
  `*_integration`) and the line for karkas.toml.
- The build (`TestService::build_program`): the project's own `build_command` — the build that makes the
  program, not the quicker `verify_command` of the agents (ADR 0029) — else Karkas's configure + build
  (CMakeLists.txt regenerated first when it is Karkas's). The same build lock as the agents' builds
  (karkas says when it waits); the timeout is at least an hour (a human waits and can stop it); nothing
  is recorded as test results. The output streams (`runner::RunOptions::on_output`, new). The build
  comes first even when the program is unknown, so the programs it made can be offered.
- Then the program replaces the karkas process (`execve`): its output, input, exit code and signals are
  its own, in a terminal exactly as from a shell. Its environment is karkas's without the variables that
  look like secrets (`runner::is_secret_name`), as for the agents' builds — the code is written by
  agents; the display and the desktop session stay, so windowed programs work. A program that needs a
  key reads it itself (e.g. `run_command = ". ~/.config/game/env && build/game"`).

karkas-gui (apps/gui/run.cpp, ui_run.cpp):
- A green triangle and a stop square at the right end of the menu bar, after the program's name (click:
  the Run tab); a Run menu: Build and run (F5), Run without building (Ctrl+F5), Stop (Shift+F5), Program
  to run…, Show the Run tab. The triangle during a run stops it and starts it again.
- The run is `karkas run` — the command a user would type, as the chat does (ADR 0019) — in a
  pseudo-terminal of its own: a pipe would make the program's C library buffer its output until it ends;
  in a terminal its lines come as it writes them, and the Run tab's input line is its stdin. `TERM=dumb`
  (no colours or cursor moves); escape sequences, carriage returns (a progress bar keeps its last state)
  and backspaces are handled as text (`TerminalText`). A session of its own (`POSIX_SPAWN_SETSID`): Stop
  sends SIGTERM to the whole process group — karkas, cmake/ninja and the compilers (reproc keeps them in
  karkas's group), the program — and SIGKILL 3 s later if it still runs. Closing the GUI or opening
  another project ends the program.
- The Run tab (a tile of the bottom panel, ADR 0032; docked with the other bottom tabs also in a layout
  saved before it existed): Run / Stop, the state (building, running with the time, the exit code, a
  failed build, stopped, nothing to run), copy and clear; the program and an editor of
  `[project] run_command` (written with `core::write_config_value`) offering the programs of the build
  folder as buttons, and "the program Karkas builds" to remove the command; the output in monospace,
  karkas's lines blue, compiler warnings yellow and errors red, followed while the view is at the
  bottom; the last 5000 lines are kept. The status bar shows a run going on.
- The program's name and the candidates are worked out by the GUI's worker (it loads the schema, karkas.toml
  and walks the build folder), when the project opens, after a run and after the command is saved.

The generated CMake writes `kk_doctest_main.cpp` with `file(CONFIGURE)` instead of `file(WRITE)`: the
file kept its text but got a new time at every configure, so every build relinked every test program
(wavkit: 8 s on each run of an unchanged project, now 0.7 s and "no work to do"). Projects regenerate
their CMakeLists.txt at their next build; the wavkit fixture was regenerated.

## Consequences

- New key `[project] run_command`; new command `karkas run`; `runner::OutputSink`, `build_command(...,
  on_output)`.
- The program does not run inside a full terminal emulator: a full-screen terminal program (curses) is
  not usable in the Run tab — run it with `karkas run` in a terminal.
- Tests: run target of a generated project (no code, the program, arguments quoted, build_dir,
  run_command), of a project with its own build (a real build: streamed, CMakeLists untouched, candidates
  without the library), a failing build command (streamed, errors kept), shell quoting, run_command in
  karkas.toml, streamed output of the runner; in the GUI with a fake karkas: phases and kinds of lines,
  a terminal for stdin (`[ -t 0 ]`, `TERM=dumb`), input, exit codes, a failed build, nothing to run,
  Stop (SIGTERM to the group with a child), a program ignoring SIGTERM killed, the GUI closing; the
  terminal text (escapes cut between pieces, `\r`, backspace, long lines); translations.
- Checked by hand: `karkas run` on a copy of wavkit (build streamed, `-- --help` passed, exit code 2 of
  the program kept), on tests/fixtures/legacy (its link error ends the run before the program), and under
  Xvfb: F5 → the Run tab with the build and the program's output, the exit code; input typed to a
  program reading stdin; a project with its own build → "nothing to run" → the editor's candidates →
  saved → F5 runs it; the Run menu in English; an older layout gets the Run tab docked.
