# 0044 — The "all levels" view: lines routed around the blocks, long chains wrapped into rows

Status: accepted (amends 14.3 all levels / groups opened in place; continues ADR 0042 and 0043)

## Context

The user reported (2026-11-21) that in "All levels" the lines looked awful while the other views were
fine. The nested picture (groups as frames around their children) had no routes: its lines were plain
curves across frames and blocks; the bends and "Arrange" routes saved for a level were applied to the same
pairs of blocks in this other geometry, turned and stretched into loops; and a project whose topics form
a chain gave a strip about 15:1.

## Decision

- The nested view (all levels, or groups opened in place) is a picture of its own: the bends of levels
  are not applied there, no bend handles are offered, sockets are on the blocks' sides.
- Its lines are routed by `layout::route_around` (kk_layout, pure geometry, tested): a Hanan grid of the
  blocks' edges kept `margin` (16 px) away, the lanes along frame borders and the middles of wide gaps; per
  line a shortest path (Dijkstra over node and direction) paying for length, turns (40), frame borders
  crossed (120) and the pieces earlier lines took (0.6 of their length per line); then the pieces of
  several lines on one line are spread `spacing` (10 px) apart, ordered by where their lines come from and
  go to, within the room the blocks leave. The first and last piece stay level at their sockets.
- karkas-gui routes when the picture changes (a hash of the sockets and blocks) and the mouse holds
  nothing; a route is used only while it still ends at its sockets. Without a route a line gets right
  angles all the same. Measured: 116 lines over 36 blocks and 9 frames in 39 ms (optimized build).
- `layout_nested` wraps the layered layout of a group whose picture is wider than 1.8 × 16:10: its rank
  columns are cut into rows stacked like lines of text, chosen to bring the whole near 16:10. Only there,
  since its lines are routed afterwards; the routes of a level stay layered.

## Consequences

- A dense graph stays dense: a note linked with everything still has a line to everything, now around the
  blocks instead of across them.
- Routes of the nested view are not saved; they are made again for each picture.
- Tests: a line round a block in its way and straight when free, a line back from the right entering on
  its side, lines through one gap spread apart, a frame border crossed once; a long chain of topics wrapped
  under 4:1 with children inside their frames and no overlaps. Checked under Xvfb: wavkit and a dense
  notes project in "All levels".
