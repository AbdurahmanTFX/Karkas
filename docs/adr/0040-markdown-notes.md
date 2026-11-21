# 0040 — Markdown notes: note.md, [text](file.md) links, a parser of its own, rendered in the GUI

Status: accepted (continues ADR 0038 and 0039; amends 0038 — a note's own text is note.md, .md files are notes)

## Context

The user asked (2026-11-21) for built-in parsing of .md files in notes mode. Asked back, the user chose:
.md files are notes (imported, and their Markdown links are links between notes), the editor highlights
Markdown, the GUI shows the markup rendered, and new notes are note.md (the note.txt files already
written keep working).

## Decision

A parser of its own (kk_core, `kk/core/markdown.hpp`, namespace `kk::core::md`) — no new dependency
(md4c or cmark would need one, and notes need a small, predictable subset):
- Blocks: ATX and setext headings, paragraphs, nested lists (bullets, numbers with '.' or ')'), task boxes
  `[ ]` / `[x]`, quotes (nested, holding any block), fenced and indented code, rules, GFM tables (alignment,
  `\|`), YAML front matter. Inline: `**bold**`, `*italic*` / `_italic_` (not inside words), `~~strike~~`,
  code spans, `[text](dest "title")`, `![alt](src)`, `[[name|shown]]`, `<autolinks>`, bare http(s):// and
  www. addresses, backslash escapes.
- Deviations from CommonMark, chosen for notes: a line break inside a paragraph stays a line break (as in
  note apps; the notes were plain text before); no lazy continuation of quotes; a nested list needs two
  more columns of indent than its parent's marker (any marker); emphasis matches the nearest fitting
  closer (no full delimiter algorithm: `*a *b* c*` differs); no reference links, HTML or footnotes (shown
  as text). Every block keeps its first source line.
- `links(text)` gives the `[label](dest)` links outside code and front matter with byte offsets;
  `plain_lines` the text without marks (the schema's preview); `is_url`, `percent_decode`.

Notes (kk_core, kk_code, kk_agents):
- A note's own text is `<block folder>/note.md`; where the loader finds only `note.txt` it keeps it
  (`Block::note_txt`). A new note — from the GUI, a line drawn to a note without text, the assistant — is
  note.md. Listed files keep their names.
- In a Markdown note (.md, .markdown) a `[text](dest)` link to a .md, .markdown or .txt file is a `ref`
  link to the note whose file it is: relative to the note's folder, `/...` from the project root, `%20`
  decoded, `#part` dropped. URLs, images, other files and links in code are no links. A link to a file that
  is no note's is W08. `[[links]]` work as before; a .txt note has no Markdown links.
- Removing a line on the schema turns such links into their text; words inside a link are no mention.
  Renaming does not touch them (they name files, not titles). A line drawn on the schema still writes
  `[[title]]` (Obsidian reads it too).
- `karkas init` and the import take .md and .markdown files (title: the file name); README, LICENSE,
  CHANGELOG and the like alone do not make a folder notes (a fresh repository stays code).
- The assistant: read_note says "format: Markdown" or "plain text"; the prompt says new notes are
  Markdown and how to use it.

karkas-gui:
- A Markdown note is drawn (markdown_view.cpp) with the draw list: word wrap, the UI font, bold / italic /
  bold italic and three heading sizes of the same family (NotoSans, DejaVu or Liberation), the mono font
  for code; task boxes, bullets by depth, quote bars, code boxes with their language, tables sized to the
  width. Links: a note's in blue (a tab opens it), web and mail addresses and files lighter blue (xdg-open
  without a shell; only http, https, mailto), a link to no note dimmed. A double click edits that line.
- A note's tab: Text | Both | Rendered (the choice is remembered: `note_view` in gui.json); Both shows the
  view beside the editor, re-parsed as one types. A new note opens where one can type. A .txt note is
  plain text as before.
- The editor colors Markdown (markdown_lang.cpp): a tokenizer of ImGuiColorTextEdit (headings, list
  markers and boxes, quotes, rules, fences, code spans, links, [[links]], bold, italic, strikethrough). The
  editor colors line by line with no state between lines, so the inner lines of a fenced block stay plain.
- The inspector shows a Markdown note rendered (up to 80 blocks); the schema shows its first lines without
  marks. The worker parses the notes, not the UI thread.

## Consequences

- Fixed on the way: a note saved from the editor grew a blank last line at each save
  (TextEditor::GetText puts '\n' after the last line too); notes are saved with `editor_text`.
- No text selection in the rendered view (ImGui has none for drawn text): copy from Text. No images
  drawn (a link to the file instead), no anchors (`#part` links do nothing), no checkbox toggling in the
  view.
- On one line, [[links]] come before Markdown links in `Block::refs` (sorted by line only).
- Tests: the parser (emphasis and what is not, links, images, autolinks, bare URLs, line breaks, headings,
  setext, lists, interruptions, quotes, code, tables, front matter, links with offsets, plain lines, URL
  and file helpers); note.md / note.txt files, link files, Markdown links as refs with W08, mentions; init
  of a Markdown folder, README alone, links removed; a new note of the assistant is note.md, read_note's
  format. Checked under Xvfb: the inspector and the three views of a tab, a link followed to its note, a
  double click to edit with the view following the typing and the note saved, a new note as note.md, a
  .txt note as before.
