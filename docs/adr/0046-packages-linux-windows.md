# 0046 — Packages for Linux and Windows; the Windows port (MinGW-w64, checked under wine)

Status: accepted (amends 18 stack, 19 build; the spec targets Linux only — Windows is a deviation)

## Context

The user asked (2026-11-21) for the GUI built as an application of its own with their icon, the CLI as a
package of its own, both for Linux and for Windows "with wine". The code was Linux-only: posix_spawn,
pipes and poll, a pty for the Run tab, inotify, flock, kill(-pgid), /proc/self/exe, XDG folders.

## Decision

**Icon.** `packaging/icons`: the rounded square cut out of the user's picture with transparent corners,
PNG 16…512 (16–64 with more contrast, the thin lines vanish otherwise) and `karkas.ico`. karkas-gui embeds
16–256 (`cmake/EmbedBinary.cmake`) and gives them to its window through GLFW (X11, Windows); on Wayland the
desktop takes the icon from the .desktop file of the application id `karkas`.

**Packages.** Two install components, each archived by the target `packages` (`cmake/MakePackages.cmake`:
`cmake --install --component` into a folder named like the archive, then tar.gz or zip; CPack's component
archives all unpack into one folder of the same name, so it is not used):
- `karkas-gui-<ver>-<os>-x86_64`: karkas-gui and the karkas CLI it runs (chat, Run tab, MCP), README; on
  Linux the .desktop entry, the hicolor icons, `install.sh` / `uninstall.sh` (into `~/.local` or `PREFIX`,
  `Exec` made absolute, what was installed listed for removal).
- `karkas-cli-<ver>-<os>-x86_64`: karkas alone.
- `cmake --workflow --preset package-linux | package-windows`.

**Linux package.** Third-party libraries fetched and linked statically (`KARKAS_SYSTEM_DEPS=OFF`), libstdc++
and libgcc static (`KARKAS_STATIC_RUNTIME`), no Graphviz (`KARKAS_GRAPHVIZ=OFF`). The system gives libcurl,
SQLite (FTS5) and OpenGL. glibc 2.43 re-versioned acosf and sqrtf; the programs are linked with
`--wrap` to their old versions (`packaging/linux/glibc_compat.c`), so a package runs on glibc 2.38 and newer.

**Windows build.** Cross-compiled with MinGW-w64 GCC (`cmake/toolchains/mingw-w64-x86_64.cmake`, presets
`windows` with tests and `package-windows`), checked under wine (`CMAKE_CROSSCOMPILING_EMULATOR`). Windows
has no SQLite or libcurl: on Windows only they are built from pinned sources — the SQLite amalgamation
3.50.4 with FTS5, libcurl 8.15.0 static with Schannel, HTTP(S) only. Each .exe is static (one file) and has
the icon, a version and a manifest (UTF-8 as the code page of narrow functions: file names, arguments,
variables; long paths). karkas-gui is a GUI program (no console window).

**Port.** The Linux code keeps its POSIX calls; Windows gets its own branches:
- `kk/core/platform.hpp`: pid, alive, kill, interrupt; UTC and local time; variables and the environment;
  home, config and data folders (Windows: `%USERPROFILE%`, `%APPDATA%`, `%LOCALAPPDATA%`, an
  `XDG_*_HOME` set still wins, for tests); create-if-absent; `FileLock` (flock / LockFileEx); PATH lookup
  (with PATHEXT); open with the desktop's program (xdg-open / ShellExecute).
- `kk/core/win_process.hpp`: `Child` — CreateProcessW with pipes, files or NUL; handles inheritable only
  around the call, under a lock; a job object with kill-on-close, so Kill ends what the command started;
  arguments quoted as the MSVC runtime splits them; .cmd/.bat run through cmd.exe; reads without waiting.
- Windows branches: the chat's command, the Run tab (pipes: no terminal), the launcher of Claude Code
  sessions, the MCP server's engines, the editor command (cmd.exe), folders and links (ShellExecute), the
  clipboard image (its "PNG" format, else the bitmap written as PNG), `karkas run` (the program as a child
  on the console, its exit code passed on), `karkas gui` (started detached), shell commands
  (`runner::shell_argv`: cmd.exe /d /s /c), `.exe` programs found by "MZ".
- The file watcher on Windows: a ReadDirectoryChangesW per folder (the folders the Linux watcher skips are
  skipped), completions on one port. A single recursive watch would do on Windows, but wine reports no
  changes inside subfolders that way.
- runner: the exit code is waited for before the stop actions (a terminate sent to a process that has
  already exited gives no exit code on Windows).

## Consequences

- On Windows Stop ends the command with what it started (there is no Ctrl+C for a process of another
  console): the agents do not finish their journals as on Linux; the Run tab has no terminal, so a program
  may print in blocks; the agents' builds need cmake, a generator and a compiler on PATH; the generated CMake
  of target projects is not checked on Windows; `claude.cmd` gets its arguments through cmd.exe.
- Tests: Linux 428 (new: the platform helpers). Windows debug build under wine: core 124/124 (incl. Child:
  pipes, exit code, input, a tree ended; quoting), layout 13/13, llm 18/18, metrics 9/9, mcp 9/9, runner
  9/9, gui 15/15 (the tests whose fake karkas is a shell script are Linux-only), index 17/18 (the 100 ms
  budget of an unchanged update: 132 ms, debug build under wine), code 41/42 (a real build of the reference
  project), agents 147/167 (fake programs that are shell scripts, git, real builds, fixture paths `/home/...`
  against `Z:\home\...`). Checked by hand under wine: `karkas check | index | query | view | init` (also in a
  folder with Cyrillic letters) and karkas-gui showing wavkit under Xvfb; the release .exe files of the zip
  archives (karkas check, karkas-gui on a notes project). They import only system DLLs and the Universal CRT
  of Windows 10.
- Sizes: Linux tar.gz 7.9 MB (gui) and 3.4 MB (cli); Windows zip 12.3 MB and 5.6 MB.
- A Windows release build in a new folder downloads every pinned source again (slow from GitHub here); the
  sources of another build folder can be given with `-DFETCHCONTENT_SOURCE_DIR_<NAME>=...`.
