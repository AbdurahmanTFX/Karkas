# 0038 — Notes mode: text notes in topics, linked by [[name]]; two workspaces

Status: accepted (extends 3 data model, 4 KSL, 14 GUI; amends 14.7 — notes are edited in karkas-gui);
amended by ADR 0040 — a note's own text is note.md (note.txt kept), .md files are notes, Markdown links

## Context

The user asked (2026-11-21) for a mode switch in the topmost bar of the IDE: a notes mode next to the
programming mode we built. Notes mode is "the same, only it stores txt files and links them together,
and everything adapts to it"; the IDE determines by itself whether a project is notes or programming.
Asked back, the user chose: links are `[[name]]` in the text (drawn on the schema; a line drawn on the
schema is written into the text), the switch gives two workspaces (each opens its last project), and
the first stage has no agents for notes.

## Decision

The model (kk_core):
- A notes project says `lang notes` in project.ksl. The tree is the same: a topic is a group, a note a
  leaf; a note's title is its card's `purpose`; its text is `<block folder>/note.txt`, or the file its
  card lists (`files`, notes that were in the folder before Karkas; a path with spaces is quoted).
- `[[name]]` (or `[[name|shown text]]`) in a note's text names a note or topic: by its id (`topic.note`
  or `topic/note`), else by its title or the last part of its id, ignoring case and repeated spaces
  (ASCII and Cyrillic). The schema loader reads the links of every note; `Schema::links()` gives one
  link of the new kind `ref` per pair. The links are never written into cards: the text is the truth.
- Checks of a notes project: missing title or kind (E02), a kind other than group/leaf (E10), a note
  holding blocks (E06), a link that names no note or several (W08, at the line of the note's file),
  the listed files (W07, E09). None of the checks of code (entry, ports, threads, guarantees, sizes of
  groups), and no drift checks (the analyzer returns nothing for a notes project).
- The index fingerprints the notes' texts: a changed text is a changed schema (its links).
- Ids from titles: Cyrillic transliterated, lowercase, words joined by `_` (`ident_from_title`).

Creating (kk_code, CLI): `init_project(root, idea, kind)`; Auto (the default of `karkas init` and the
GUI) makes a notes project of a folder that holds `.txt` files and no code. A notes project gets only
project.ksl, blocks/ and .karkas/ — no karkas.toml, CMake, guide or subagents — and `import_notes`
writes cards for the `.txt` files already there: each folder with notes a topic, each file a note
listing it (the files stay where they are; hidden folders, build trees and blocks/ are skipped; run
again it takes only files no card lists). `karkas init --notes | --code` choose explicitly. Edits of
notes as plain file edits: `add_note_link` (a new last line `[[title]]`, the file made if missing),
`remove_note_link` (links become their plain text), `retitle_note` (the card's purpose; links that
named the note by its old title follow, links by id stay).

karkas-gui (ui_notes.cpp and the places that adapt):
- "Programming | Notes" as the first two items of the Karkas menu, the current one checked (at first tabs
  in the menu bar after the menus; moved into the menu at the user's request, 2026-11-21). The open
  project's kind sets the mode.
  Each workspace keeps its last project (`last_code`, `last_notes` in gui.json; the recent list is
  shared, up to 16); the other tab opens its last project, else its picker — the recent projects of
  that kind, or the Create tab when there are none. Create chooses Programming / Notes; an existing
  folder of notes selects Notes by itself.
- A notes project shows topics and notes by their titles (tree, schema, tabs, inspector), "topic" /
  "note" for the kinds, a note's first lines on its block, a topic's number of notes; no status dots.
  The toolbar has + Note, + Topic, All levels, Undo, Fit, Up; the background and block menus offer new
  notes and topics, edit, rename, remove. Adding uses the KSL edit (`op_add_block` without a thread)
  applied at once — no preview, undo keeps it; a new note opens in an editor tab with the keyboard.
  Removing asks first and can be undone (the card goes; the text stays in its folder, so Undo brings
  both back; a listed file stays where it is). Renaming changes the title and the links by it.
- A note's text is edited in an editor tab (ImGuiColorTextEdit, plain text): saved by itself 1.5 s
  after the last keystroke, by Ctrl+S or the Save button, when its tab closes and when the window
  closes; the tab marks unsaved text with ●; a file changed on disk reloads when nothing is unsaved.
  A note not written yet opens empty.
- A line drawn from a note's point to another note or topic writes `[[title]]` into the first note;
  removing a line makes its links plain text.
- The inspector of a note: title, its topics, Open / Rename / Folder / Remove, the text (wrapped), its
  links and the ones that name nothing (W08), the notes that link to it; of a topic: its notes and the
  notes that link to it. Search covers titles and texts, with the line found.
- Hidden in a notes project: Run (menu, buttons, tab, F5), Build & tests, Agents, Costs, Tickets, the
  chat and its activity button, schema changes, prototype, model chip, "use links", the agents' status
  items. The Drift tab is "Problems".

## Consequences

- No word wrap while editing: ImGui 1.91.9 and ImGuiColorTextEdit have none (the inspector shows the
  text wrapped). A later ImGui with `ImGuiInputTextFlags_WordWrap` would allow it (a dependency update).
- Agents for notes (write, link, sort into topics) are the next stage; MCP and the CLI see a notes
  project's schema and links already (`karkas view`, `karkas check`).
- Undo of the GUI covers adding and removing notes and topics; text edits, links drawn and renames
  are file edits without undo.
- Tests: links of a text (lines, labels, not across lines), titles folded and ids from them, the loader
  and ref links (by title, by id with '/', a listed file with spaces, a topic), checks (E10, W08 none and
  several, no checks of code, an empty topic), quoted paths written and read back; init detecting notes,
  topics and notes from folders and files, files untouched, a second import only new files; a code
  folder and an empty one stay code, notes asked for; links added (file made) and removed, a title
  changed with the links by it; no drift for notes; translations.
- Checked under Xvfb: a folder of notes → `karkas init` → the notes mode (tabs, titles, previews, the
  Problems tab with a broken link); a topic entered, a note opened and typed into (Cyrillic, saved by
  itself, links and backlinks in the inspector); + Note → typed at once → saved; a line drawn → `[[...]]`
  in the text; a rename followed by the link; remove and undo in one session; "Programming" opens the
  last code project with its full interface; "Notes" without a notes project → its Create tab → a new
  notes project opens in the notes mode.
