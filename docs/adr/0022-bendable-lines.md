# 0022 — Lines of the schema can be bent by hand

Status: accepted (amends 14.6 schema canvas, 0021)

## Context

The node editor draws every link as one cubic curve between its two pins; on a busy level lines
cross blocks and each other, and the user could not choose a path from A to B.

## Decision

- Hovering a line shows handles: a "+" in the middle of each piece of the line, and the points it
  already bends at. Dragging a "+" bends the line there (a new point); dragging a point moves it;
  a double click on a point removes it. The line's menu (right click) has "Straighten the line",
  the background menu "Straighten every line of this level".
- A bent line is drawn by karkas-gui, not by the editor: a smooth curve (Catmull-Rom) through its
  points that leaves and enters along the sockets. It lies on the editor's underlay channel (over
  the grid, under lines and blocks), so it passes under blocks like the editor's lines; arrows
  that would lie under a block are not drawn.
- The ends of a bent line attach on the side of their block that faces the nearest bend.
- A bend is kept as (u, v) in the frame of the centers A, B of the line's two blocks — u along
  A→B, v across it, in lengths of AB — so moving a block moves, turns and scales the curve with
  it, and a change of the side a line leaves on does not move the bend.
- The key of a line is its kind and its two blocks (data and event lines also their ports):
  `call|app|rules`, `data|a|b|a:out>b:in`. A line between the same two blocks keeps its bends on
  every level where it is drawn between them. Stored in `.karkas/bends.json`.
- Handles are items of the canvas submitted before the editor's own: a pressed handle holds the
  mouse, so the editor neither pans nor selects; the same item id is submitted every frame of a
  drag. The editor still draws, selects and deletes the lines that are not bent.

## Consequences

- A bent line is not an editor link: its tooltip and menu come from karkas-gui; it cannot be
  selected and removed with Delete (its menu removes it).
- The plan of lines is made again whenever a bend changes (also during a drag).
