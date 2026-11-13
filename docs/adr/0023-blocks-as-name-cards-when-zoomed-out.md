# 0023 — Zoomed out, a block is a card with its name

Status: accepted (amends 14.6 schema canvas, 0021 level of detail)

## Context

Zoomed out (a big level, "All levels") the text inside blocks became too small to read; 0021 only
stopped drawing port names below ~4 px, and the block's own name stayed tiny.

## Decision

- Per block, by the size its text would have on screen: below ~8 px of line height the block is a
  card — the band of its color over the whole block and its name alone, centered, as large as the
  block allows (86 % of its width, 78 % of its height). Zooming in past that size brings back the
  inside: kind icon, name, status, purpose, ports with their sockets.
- A block with larger text (made bigger by hand) stays detailed longer than a small one.
- The pins keep their places in both forms, so lines do not move when a block switches; on a card
  the sockets, the resize grip and the tooltips of its pins are not shown.
- The name is drawn with the medium font; when it is shown larger than its small raster, a second
  raster of the same font (28 px) keeps it sharp (`theme::display_font`).

## Consequences

- At the default fit of a busy level the user sees names and lines; ports are one zoom step away.
- Borders still show the state (failing, drift, needs review, working agent) on cards.
