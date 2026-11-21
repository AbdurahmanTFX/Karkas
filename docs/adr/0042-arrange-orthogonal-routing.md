# 0042 — "Arrange": a better layered layout and lines routed orthogonally on tracks

Status: accepted (amends 14.2 picture layout and ADR 0002 — the built-in engine; extends ADR 0022 — bends)

## Context

The user asked (2026-11-21) for an algorithm that connects the lines between blocks properly, with a
structure instead of a tangle, and a button that arranges the blocks and the paths of the lines. The
built-in Sugiyama engine (ADR 0002) returned block positions only: a line over several ranks was drawn
as one curve across the blocks between, a pair of notes linking each other made a cycle, and all lines
of a block side met in one point. Graphviz is not installed here, so the built-in engine is what users get.

## Decision

The built-in engine (libs/layout/src/sugiyama.cpp), every step a known method, deterministic:
- A pair of blocks linked both ways is one structural edge, in the order of the blocks (notes that link
  back and forth read in their own order).
- Cycles: the greedy order of Eades, Lin and Smyth (sinks to the end, sources to the front, else the
  block with the most outgoing over incoming); edges against it are turned round.
- Ranks: the longest path, then each block pulled toward the side it has more lines to (a source next to
  its first successor), which shortens long lines.
- Long lines are concentrated: the dummies of the long lines of one block form a trunk — out of a block
  with many of them, into a block that many of them reach — and each line leaves the trunk at its rank.
- Order inside ranks: a depth-first start, 24 sweeps of weighted medians with transposition (neighbors
  swapped while that removes crossings); crossings between ranks counted with a Fenwick tree; the best
  order kept.
- Heights: per rank a weighted isotonic regression (pool adjacent violators) of the wanted heights under
  the order and the gaps — each vertex wants the mean of its neighbors' attach points (lines between
  dummies weigh most); a block's lines attach at `Node::anchor` (the GUI gives the title line, where block
  points sit), so linked blocks line up there; then trunks are made straight.
- Routes (`Layout::routes`, one per edge): orthogonal. A line leaves its block level, turns at a track of
  its own in each gap between ranks and crosses the columns it passes over between the blocks there. In a
  gap the order of the turns is chosen pair by pair to cross least; turns that do not overlap share a
  track; a gap is as wide as its tracks need. Lines of one pair lie side by side.
- Connected parts are laid out alone and packed on shelves (largest first), single blocks in rows under
  them, within a width that keeps the picture near 16:10.
- The cache key of a layout carries the engine's version (`-v2`): cached layouts of the old engine are not
  reused, so levels without manual positions get the new layout.

karkas-gui:
- "Arrange" on the toolbar of the schema (both modes) and in the background menu: the level is laid out
  by the built-in engine with the blocks' current sizes and the title line as anchor; positions are saved
  as manual ones (manual sizes kept); every line of the level gets the corners of its route as bends and
  is marked routed (`"#ortho"` in `.karkas/bends.json`; an older Karkas ignores the key). The arrow next
  to it puts positions, bends and marks back (one step, until the level changes). Not offered while
  groups are opened in place.
- A routed line is drawn as level and upright pieces with rounded corners (cubic pieces, so hover, menus
  and flow arrows work as for other lines); its first and last piece run level from its real sockets.
  When its corners lose their right angles (one of its blocks moved alone), it is drawn as a smooth curve
  through them; dragging one of its corners makes it a free line; "Straighten" clears it.

## Consequences

- Dense graphs stay busy (a hub linked with every block draws a line to every block), but as trunks and
  tracks instead of crossing curves; a long chain of blocks gives a wide picture.
- All lines of a block side still attach at one socket; routes leave it as a bus. Spreading sockets along
  the side is not done.
- Routes are computed on "Arrange" only; the automatic layout of a level without manual positions uses
  the new engine for positions and draws plain curves.
- Tests: a long line straight across the columns it passes, orthogonal routes clear of every block, the
  order inside ranks removes avoidable crossings, a pair linked both ways is one edge without loops, a
  line against the flow routed back between the blocks, single blocks in rows under the linked part, a
  fan of lines on separate tracks without crossing each other, determinism. Checked under Xvfb: the
  wavkit fixture (top level with a line back, the dsp level with a fan of calls), a copy of a dense notes
  project, undo of an arrangement, a block moved after it.
