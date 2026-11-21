# 0045 — Mouse on the schema: locked and free

Status: accepted (extends 14 GUI — schema canvas; ADR 0022 bends, 0035 prototype, 0042 Arrange)

## Context

The user asked (2026-11-21) for two mouse modes on the schema: a locked one, where nothing can be done but
moving around the schemas and their levels, selecting blocks and looking at what they hold; and a free
one — the schema as it was.

## Decision

- `schema_locked` in gui.json (default false: free). Switched by a toggle at the start of the schema's tool
  bar in both modes (a padlock: "Locked" lit / "Free"), the View menu, the background menu and Ctrl+L.
- Locked keeps: panning and zooming, Fit, entering a group (double click, "Open"), the address bar and Up,
  All levels and groups opened in place (Shift+double click), closing a frame by its path bar, selecting
  (click, rectangle), Shift+click highlighting, hover tooltips, opening a note's text or a block's code by
  a double click, Right angles and Schema only (they change the view, not the project), opening the
  card or the folder of a block.
- Locked stops: dragging blocks, resize grips, drawing links from points, bend handles (moving, adding,
  removing bends), Delete and Alt+click on lines, the items of the block, line and background menus that
  change the project or the picture (rename, add port, split, merge, reset size, remove, new note/topic,
  add block, Arrange, auto layout, straighten), and on the tool bar Undo, Add block / Note / Topic,
  Arrange and its undo, Prototype (locking turns the prototype off; its sketch is kept). A line's menu
  shows what the line carries. Dragging a block or a point shows a "locked" cursor and a hint.
- The node editor has no switch for dragging: locked, its drag button is set (through its internal
  header, as `fit_canvas` does) to a mouse button it never presses its items with, so a press selects and
  nothing moves or starts a link; a double click on a block is then taken by karkas-gui itself.

## Consequences

- Only the schema is locked: the inspector, the code and note editors and the explorer still edit as
  before.
- Tests: the setting round-trips and defaults to free; every new text has its translation. Checked under
  Xvfb (wavkit): free — a dragged block's position is saved; locked — the same drag only selects it and
  nothing is saved, a drag from a point proposes nothing, a double click opens the group, the tool bar
  greys its editing buttons, the background menu offers view items and "Free the schema"; notes mode
  likewise.
