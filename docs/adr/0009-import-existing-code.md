# 0009 — Importing existing code: blocks over files that stay in place

Status: accepted

Stage 11 lists "import of existing projects (cutting into blocks by the code graph and
clustering, then approval by a person)". Implemented a deterministic first version.

## Blocks over existing files

Existing projects keep several modules in one folder (`src/active/cat.cpp`,
`src/active/hpp/cat.hpp`, ...), while a block of the spec is a folder with api/ impl/.
Moving code would break the project's build and history, so:

- New card directive `files <path>...` (leaf, entry, shared): project-relative files that
  belong to the block. Headers are its api, other code files its impl. The card itself
  lives in `blocks/<id>/block.ksl` as usual; the code stays where it is.
- E09: listed files exist, belong to one block only and are not inside another block's
  folder.
- Imported code often includes another module only for its types; a `use` of an imported
  (non-shared) block is therefore allowed (E03 is not reported for it).
- Drift checks work on the listed files: includes resolve relative to the file or by a
  unique path suffix (quoted includes only); D04 compares names as KSL identifiers
  (`ResolveCollision` in C++ is `resolve_collision` in the card); D08 and W06 are skipped
  for imported blocks (no contract tests, no invariants yet). An imported block with code
  is `implemented`.
- Generation (skel, mocks, probe, cmake) never touches imported blocks: the project keeps
  its own CMakeLists.txt.
- Agents refuse to work inside imported blocks for now (their sandbox is the block folder
  with api/ impl/); this is the next step.

## karkas import

No model calls. Folders become groups (a leading `src/` is dropped, chains of single
subfolders fold); files of one area with one stem become a leaf (header folders like
`hpp/`, `include/` are ignored; a lone header and a lone source with a unique common stem
pair up; a source that only defines what one module's header declares joins it); `main()`
is the entry (no main: an empty `app` entry, the schema needs one); header-only modules
without calls are shared. Public functions and methods of the headers become fn ports
(the ones other blocks call first, at most 24, the rest named in a note); calls become
`call` links, includes without calls become `use` links — by the same rules as the drift
checks, so the imported schema has no D02/D09 right away. The first comment of the header
(its first sentence, ≤ 100 characters) is the purpose; otherwise a placeholder and a note.
Skipped: third_party, vendor, tests, build folders, `--exclude`.

The result is a @change (`karkas approve C-NNNN` or the Changes tab of the GUI), or
`--apply` / `--dry-run`.

Known limits: method calls are matched by name among included modules (no type
inference), so a call `x.Update()` inside a class that has its own `Update` is not linked;
no clustering of big folders yet; purposes from comments are a heuristic and an optional
`--describe` with a cheap model (8.x) is not implemented.

`karkas init` in a folder with code keeps the C++ standard of CMakeLists.txt, writes no
generic rules and suggests `karkas import`.
