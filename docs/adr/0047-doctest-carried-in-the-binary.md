# 0047 — doctest.h carried in the binary for target builds

Status: accepted (fixes 12.1 / 18 in packages; ADR 0014 KARKAS_DOCTEST_DIR, 0046 packages)

## Context

A check of the v0.1.0 Linux package on another machine (2026-11-22): `karkas test` failed on wavkit with
`doctest/doctest.h: No such file`. kk_agents had the doctest folder of the build tree compiled in
(`KK_DOCTEST_INCLUDE_DIR`, here `/home/tfx/.../build/package-linux/_deps/doctest-src`) and Karkas passed it
as `-DKARKAS_DOCTEST_DIR` to every configure. Elsewhere the folder does not exist, yet the generated CMake
took any non-empty value, so neither the system's doctest nor FetchContent was tried; the environment
variable did not help, the `-D` came first. Testers, workers and `implement` all build through this.
The package also carried a folder name of the build machine.

## Decision

- kk_templates embeds `doctest/doctest.h` of the build (the pinned FetchContent copy, or the system's) as
  `vendor/doctest/doctest.h`. `cmake/EmbedFiles.cmake` takes list lines `<path>=<absolute file>` for files
  outside templates/, and the table is built from `""sv` literals (a 300 KB literal made the constexpr
  strlen exceed GCC's loop limit).
- `TestService::doctest_include()`: the build tree's folder when it still holds `doctest/doctest.h`
  (developer builds, `KARKAS_TESTS=ON`: tests stay offline and write nothing outside their folders), else
  the embedded copy written once to `<data_home>/karkas/doctest-<major.minor.patch>/doctest/doctest.h`
  (`~/.local/share`, `%LOCALAPPDATA%`; rewritten when it differs), else "" (nothing is passed).
- `KK_DOCTEST_INCLUDE_DIR` is defined for kk_agents only when `KARKAS_TESTS` is on: packages
  (`KARKAS_TESTS=OFF`) carry no doctest folder of the build machine and always use the embedded copy.
  (`karkas bench` still has the source folder as its default for bench/ and the reference fixture: a
  developer tool that needs the source tree anyway.)
- The generated CMake (cmake_preamble.cmake) treats a `KARKAS_DOCTEST_DIR` without `doctest/doctest.h` as
  unset (a status line says so) and goes on to `find_package(doctest)` / FetchContent. A build folder
  configured by v0.1.0 keeps the bad path in its cache; the next build by Karkas passes the good one.

## Consequences

- The binary grows by about 330 KB. doctest is MIT; the written header keeps its copyright notice.
- No new dependency: doctest was already pinned (section 18).
- Tests: the resolver (build tree, missing build tree → embedded copy with the same bytes, kept,
  rewritten when damaged, nowhere to write → ""). Checked by hand: with the build tree's doctest moved
  away, `karkas test` on a copy of wavkit whose build folder was configured with a nonexistent
  `KARKAS_DOCTEST_DIR` builds and passes (9 blocks); a configure with a bad folder falls back to
  FetchContent.
