# 0008 — GUI settings, interface language and block sizes

Status: accepted

Section 14 does not mention an interface language, and 14.2 says the node size is
computed in advance from the text width. Added on request:

- Interface language: English and Russian (apps/gui/i18n.cpp, keys are the English texts).
  Window and popup titles use "Text###StableId", so the docking layout does not depend on
  the language. Schema data (ids, purposes, KSL) is shown as written. A test checks that
  every `tr("…")`/`title("…")` text of the GUI sources has a Russian translation.
- User settings live outside the project, in `$XDG_CONFIG_HOME/karkas/gui.json`
  (default `~/.config/karkas/gui.json`): `language`, `block_scale`, `ui_scale`. The default
  language comes from LC_ALL / LC_MESSAGES / LANG. They are personal preferences, so they do
  not go to karkas.toml (which is in git and shared by the team). Headless runs
  (`--frames`) never write the file.
- Block sizes: the size from 14.2 is still the default, multiplied by `block_scale`
  (0.6–2.5). A block can be resized by its bottom-right corner; the size is saved next to the
  manual position in `.karkas/layout/<view>.json` as `[x, y, w, h]` (old `[x, y]` entries are
  still read; "size 0" = automatic). The layout engine gets the actual sizes, so automatic
  layout never overlaps blocks.
- Limits: a block is never smaller than 120 × 56 canvas units (and never larger than
  1600 × 1200), so it cannot shrink to a dot.
- Text follows the size: font scale = min(width / default width, height / default height),
  clamped to 0.55–2.5; line spacing scales with it; lines that do not fit end with "…";
  ports that do not fit fold into "+N" pins (links to hidden ports attach there).
