# 0010 — GUI: link colors, focus, groups opened in place, address bar, projects

Status: accepted

Additions to section 14 on request; one deviation.

- Deviation from 14.5: links are colored by the block they start at (well-separated hues
  per level), because many lines of one kind (calls of imported code) were all gray. The
  colors of 14.5 (data blue, event orange, call gray, state red, use thin gray) remain as
  the setting "Link colors: by link kind"; thickness still tells state (thick) and use
  (thin) apart.
- Focus: hovering a block (schema or tree) dims everything except it, its links and its
  partners; Shift+click keeps a block's links lit, several blocks can be lit (Shift+click
  again, Esc, a click on the background or "Clear" turn it off).
- Groups opened in place (Shift+double-click): the group's whole subtree is laid out inside
  a frame among the closed blocks of the level (kk_layout::layout_nested), so it shows which
  inner blocks talk to which whole blocks. The frame's top edge is a path bar; a click on a
  part closes that group. The view is temporary: positions are not saved there.
- Pins flow left to right like the layout: inputs on the left (in, fn, "used"), outputs on
  the right (out, "calls"), so call lines no longer cross the blocks.
- Address bar "project / group / group" instead of breadcrumb buttons; the empty part turns
  into a text field for a typed path.
- Projects: "Open project…" (Ctrl+O, a folder browser that marks folders with project.ksl,
  recent projects in ~/.config/karkas/gui.json); started outside a project, karkas-gui opens
  with it. "Show project folder" / "Open folder" of a block use xdg-open.

## Addendum — IDE layout and look

- Block backgrounds take the block's own color (title band and a tinted body); the status
  of 14.5 is a colored dot on the block, and problems (failing, drift, needs_review, an
  agent at work) are shown on the border. The colors of 14.5 for backgrounds remain as the
  setting "Block colors: by status". Second deviation from 14.5, same reason as for links.
- "All levels" (14.3) is drawn as every group opened in place (frames in the node editor)
  instead of a separate picture of nested boxes, so zoom, colors, focus and links with
  ports work there too.
- Layout after VSCode and Godot: activity bar, side bar views (explorer, search, schema
  changes, settings), editor tabs (schema and code files), inspector, bottom panel, status
  bar. The panels of 14.4 are all there; "Changes" moved from the bottom tabs to the side
  bar (like source control in an IDE).
- No new dependencies: icons are signed distance fields drawn in code and rasterized into
  the ImGui font atlas; fonts are taken from the system at run time (Noto Sans, JetBrains
  Mono; DejaVu as fallback).
