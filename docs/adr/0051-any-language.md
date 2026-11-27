# 0051 — Any language: code of every language is blocks, facts and checks, not only C/C++

Status: accepted (extends ADR 0009 import, 0016 imported projects, 0027 agents inside imported blocks;
deviates from spec 8.1, which names tree-sitter C++ as the only analyzer)

## Context

Karkas analyzed C and C++ only: `core::is_code_file` knew ten extensions, the index, the drift checks,
import and the agents' sandbox took "header" for "contract". A project in another language could be
described by hand (cards with `files`), but its files were data: no facts, no `kk_locate` symbols, no
D02/D04/D09, status `design` forever, and `karkas import` saw nothing. The first real case was a
Hyprland desktop config (C++ panels, 18 shell scripts, `hyprland.lua`, one Python helper): 16 of its 33
blocks stayed `design`, and the links between the scripts were whatever a human remembered to write.

## Decision

- **Languages are data** (`kk/core/lang.hpp`): 28 languages with their extensions, comment and string
  syntax, interface files and whether the interface lives in separate files. C/C++ (`split_interface`:
  headers are the api) stays as it was. Every other language keeps no separate interface: a file is its
  own contract, its public names are the api (`.pyi`, `.d.ts`, `.mli`, `.fsi`, `.hrl` are interface files
  where a language has them). `core::is_code_file` is "a known language".
- **`lang` names the project's languages**: `lang cpp bash lua python`. Orphans (D06), `karkas import`
  and `karkas init` take the code of these languages; a C++ project's Python build helpers are no
  orphans of it. An empty or unknown `lang` means C/C++, so existing projects behave as before.
  `karkas init` writes the languages it finds, most files first, and the C++ build lines only where
  there is C++.
- **Lexical analysis for every language but C/C++** (`kk/code/lexical.hpp`), with the same facts as the
  tree-sitter walker: comments and strings are masked by the language's syntax (shell heredocs, Lua long
  brackets, triple quotes), then rules per language find declarations with their scope (braces, or
  indentation for Python, Ruby, Lua, Nim ...), parameter count (without `self`, `&self`, a Go receiver),
  and visibility by the language's own rule (`export`, `pub`, a capital in Go, `public`, no leading `_`,
  not `local`, `def` vs `defp` ...). Imports (`import`, `from`, `require`, `use`, `source`, `mod x;`) and
  called names become refs. A shell script's subcommands — the labels of its top-level `case "$1" in`,
  also through `ACTION=$1` — are public symbols of kind `command`: a script's api is its command line.
  A program's entry (`__main__`, `static void main`, `@main`, `fn main`) is a `main` symbol for import.
  No new dependency: tree-sitter grammars for more languages can replace a language's rules later behind
  the same `FileFacts`.
- **Imports resolve by the rules of each language** (`code::resolve_import`): Python dotted and relative
  modules (`__init__.py`), JS/TS relative paths with their extensions and `index.*`, Go import paths to a
  package folder (only paths with a domain, never the standard library), Rust `crate::`/`super::`/`mod x;`,
  Java/Kotlin/Scala packages, Lua `require`, Ruby `require_relative`, PHP, Perl, Haskell, Nim, Dart, and a
  shell `source ~/.config/x/y.sh` by the longest tail that names one project file. A suffix that fits
  several files resolves to none, as for C++ includes. The drift check uses them for D01/D02 ("imports X
  api without link"); a file of a language without headers in an imported block counts as that block's
  api.
- **Checks**: exported symbols are those of api files or marked exported; D04 accepts subcommands and
  skips the parameter count where the language does not say it (shell: -1). An imported block whose
  listed files exist but hold no code (a `dunstrc`) is `implemented`, not `design`.
- **Import**: one leaf per file; a Go package (folder) is one leaf; `index.ts`, `mod.rs`, `__init__.py`,
  `init.lua` are named by their folder; fn ports from the public names with `any` per parameter;
  links from imports and calls as for C++. Test programs: also `foo.spec.ts`, `foo_spec.rb`,
  `test-foo.sh`, `conftest.py`.
- **Agents in imported blocks**: a file of a language without headers is the contract other blocks read
  (sandbox, brief, neighbor declarations); writing it keeps its public names and their parameter counts
  (`agents::contract_kept`, the counterpart of `header_contract_kept`); a new public function, type or
  subcommand is a card change. Only C/C++ files go into a CMakeLists.txt or are compiled alone by
  `build()`; other languages are checked by the project's `build_command` and `[test] command`.

## Consequences

- An imported project in any of the 28 languages gets blocks, facts, `kk_locate` symbols, D01/D02/D04/D06/
  D09 and statuses; its agents edit its files in place under the same contract rule as C++ headers.
- The lexical rules are heuristics: a declaration that a language writes in an unusual form (a Python
  function made by a decorator factory, a C# method with attributes on the same line as a lambda) can be
  missed or mis-scoped. D09 stays a warning; D04 compares names and counts, never types, outside C/C++.
- Not in this ADR: schema-first (native) projects in other languages. `karkas approve api` still
  generates CMake, doctest contract tests, mocks and skeletons for C++ only; a native project in another
  language has to bring its own `build_command` and `[test] command` (stage 2 of this work).
