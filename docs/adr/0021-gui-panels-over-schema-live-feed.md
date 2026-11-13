# 0021 — GUI: panels over the schema, live feed of agents, ghost of a change, a faster canvas

Status: accepted (amends 14.1 shell layout, 14.6 schema canvas, 0019 chat and panels)

## Context

Using karkas-gui on flappy-karkas while DeepSeek agents worked (2026-11-12) showed:

- The schema was one docked window among others: a panel shown, hidden or resized changed the
  schema's size, and the chat window could dock into the layout. Panel sizes were sometimes read
  in the frame after a rebuild and saved as 8 % of the window.
- Text in the chat could not be selected: a click first turned a text into a read-only field;
  results (KSL), progress lines and the steps of the agents could not be selected at all.
- In the changes panel a row moved when the mouse came over it (its hide button was placed with
  `SetCursorScreenPos` into the flow of rows) and the hide button vanished under the mouse (it was
  shown only while the row itself was hovered).
- The pointing hand was the old X11 `hand1` (finger to the upper right): GLFW 3.5 on Wayland asks
  libwayland-cursor for the theme "default", whose index.theme (written by nwg-look) inherits
  `Future-black Cursors` — a display name with a space, which that library splits into two theme
  names and never finds, so it falls back to its built-in X11 set.
- "All levels" of a project with 112 blocks cost ~52–62 ms of CPU per frame (debug build): a scan
  of every port per block (`pin_rows`), a scan of every line per port (event sockets), and per
  frame ~10 string allocations per port and per link.
- Links attached on fixed sides; a selected change could not be seen on the schema.

## Decision

Layout (ui_shell.cpp):
- The schema covers the whole work area and stays at the back; the side bar, the inspector and the
  bottom panel lie on its edges (`begin_panel`) and are resized at their inner border, kept as
  parts of the work area (`side_frac`, `inspector_frac`, `bottom_frac`, saved when a drag ends).
  The editor's tab bar and toolbar lie over the schema in the part no panel covers. No docking;
  windows move by their title bar only. The chat floats above everything (`NoDocking`).
- Fit to view puts the blocks into the free part (`fit_canvas`: the view rectangle is computed
  from the insets, through the node editor's internal `NavigateTo`).

Chat (selectable.hpp): every text of the dialog and of the live feed is a piece of one selection
— press and drag (over several pieces, the region scrolls at its edge), double click a word (block
ids, tickets and paths are one word), triple click a piece, Shift+click extends, Ctrl+A, Ctrl+C,
context menu (copy the selection or the whole message). A piece is an item that holds the mouse
while pressed, so the window never moves under a drag. KSL keeps its colors (`ksl_spans`).

Live feed (feed.hpp): every step of any run journal — what the model thought, the tools it called
with their arguments, what came back — read incrementally, also for runs started from a terminal;
an active agent in the panels or the status bar opens its run.

Changes: a row's hide button is submitted before the row (it gets the click), drawn after it (over
the highlight); the row is hovered by its rectangle, also while the button is held. A selected
change opens the level that shows it (once per choice) and draws its ghost: new blocks dashed
green, added and removed ports and links, touched blocks outlined; a decided change shows its
blocks only. The diff can be edited in place (checked before it is saved).

Canvas (ui.cpp):
- Lines attach on the side facing the other end; ports go to the left or right column by the side
  most of their lines go to; block points can be on any side. Socket shapes tell the kind: circle
  data, diamond event, square a function, arrow a call, open diamond use, ring state. Chevrons on
  each line show the direction; they run along the lines of the block in focus.
- What does not change between frames is computed once: the plan of lines (sides, bundles, pin
  ids) per model, block positions and sizes, ghost and "use links"; the port rows of each block;
  the unit of each block and the used ports per model; port lists per block. Bundle labels and
  link descriptions are made only for a tooltip, a menu or a delete. Port names and purposes
  smaller than ~4 px on screen are not drawn (their place is kept, so pins do not move with the
  zoom). Measured: "All levels" of blobby (112 blocks) 52–62 ms → 17–19 ms per frame in the debug
  build. `KARKAS_GUI_PROFILE=1` prints the frame time and the parts of the schema canvas.
- Caches are reset when another project is opened.

Cursors (cursors.hpp): the theme is found as a person means it — `XCURSOR_THEME`, else "default"
and what it inherits, an inherited name matched against folder names and the themes' own `Name=`
— and its Xcursor images become GLFW cursors for every ImGui cursor (GLFW's own where the theme has
none). The hand points like everywhere else on the desktop.

Also: tickets show their title first (time, cost and kind only when the title keeps 170 px);
fonts load Greek, math operators (≤ ≥ ≠), letterlike and technical signs from DejaVu.

## Consequences

- A panel never changes the schema; parts of the schema can be under a panel (fit avoids it).
- The canvas keeps per-model caches; a new kind of per-frame state must be added to the plan's
  signature, or it will not be recomputed.
- `canvas_fit.cpp` includes the node editor's internal header (pinned version, 14.1).
- Tests: `apps/gui/tests/cursors_test.cpp` (theme found by its Name=, Xcursor read and
  un-premultiplied), `feed_test.cpp`; the canvas, the selection and the layout were checked with
  headless screenshots (`--script`).
