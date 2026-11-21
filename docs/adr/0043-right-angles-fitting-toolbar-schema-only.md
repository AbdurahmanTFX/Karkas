# 0043 — Lines with right angles, a tool bar that fits its panel, the schema alone

Status: accepted (amends 14 GUI and ADR 0042 — gaps between routed lines; extends ADR 0022 — bends)

## Context

The user asked (2026-11-21) for: lines with right angles; tool bars whose buttons do not vanish in a
narrow window — texts going first, icons staying, and the window not narrower than that; wider gaps
between the branches "Arrange" lays out (they ran together like one branch); a button that hides
everything but the schema.

## Decision

- **Right angles** — a switch (the schema's tool bar, View menu, background menu; `square_lines` in
  gui.json). Every line is then drawn by karkas-gui (not the node editor, as bent lines are): from its
  socket along the socket's axis, to each of its bends along one axis and then the other, into the far
  socket along its axis — a step half way when both sockets face the same axis, one corner when not.
  Corners are rounded (12 px), pieces are cubic, so hover, menus and flow arrows work as for any line.
  The "+" handles sit in the middle of the longest piece between two points; a new bend goes between
  them. A line routed by "Arrange" keeps its exact corners; when they lose their right angles (a block
  moved alone) it is drawn with right angles through them in this mode, as a smooth curve otherwise.
- **A tool bar that fits** (`ui::tool_row_begin/end`, `tool_fit`): while a tool bar is drawn, icon
  buttons and toggles know whether the bar with all its texts fits the panel (the width it needed with
  texts is kept from the last time it was drawn so); when not, they show their icons only and their text
  goes into the tooltip. An item that does not fit on the row starts a new row. Panel titles (Explorer,
  Inspector) are shortened with "…" instead of running under their buttons. The window keeps at least
  600 × 420 (times the interface scale).
- **Schema only** (a tool bar button, View menu, F11): hides the side bar, the inspector, the bottom panel,
  the activity and status bars and the chat; the menu bar and the schema's tool bar stay. Again: they come
  back as they were. Settings saved meanwhile keep the panels as they will come back, and closing the
  project restores them.
- **Wider gaps** in the layout: parallel lines (dummies) 30 px apart, a line and a block 30 px or more,
  tracks of turns 20 px apart, 34 px from a column to its first track, lines of one pair 10 px apart.
- For headless checks karkas-gui takes `--size WxH`, and the input script `key f11`.

## Consequences

- With right angles every line is karkas-gui's: lines are selected and removed through their menu (right
  click), not with the editor's selection and Delete — as bent lines already were.
- Texts go all at once (no order of importance); a very narrow schema panel shows its buttons on two rows.
- Lines of one socket still share their first piece (a bus); spreading sockets along a side is not done.
- Checked under Xvfb: right angles on the automatic layout (wavkit), the schema alone with F11 and back,
  the tool bar at 1100, 760 and 600 px (texts gone, icons, a second row), panel titles in a narrow window,
  "Arrange" on a dense notes project with the wider gaps. Tests 422 (the layout tests hold with the new gaps).
