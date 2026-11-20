# 0032 — GUI: tiles in a dockspace, floating tiles, tiles as windows of the desktop

Status: accepted (amends 14.4 panels and ADR 0021 "no docking; the chat floats above everything")

## Context

The user asked (2026-11-20) to pin the orchestrator chat on the screen as a tile of the main window,
to turn the other tiles (side bar, inspector, bottom tabs) into floating windows, and to make them
separate windows of the operating system, like applications of their own. ADR 0021 had fixed the
panels to the edges of the work area with `NoDocking` everywhere, because a docked schema changed
its size whenever a panel was shown, hidden or resized.

The user's desktop is Hyprland (Wayland). A Wayland client never learns the global position of its
windows: `glfwGetWindowPos` returns 0 and `glfwSetWindowPos` is refused. Dear ImGui's
multi-viewports keep every window of the desktop in one coordinate space and compute the mouse as
"window position + position inside it"; on Wayland every window would be at 0,0, the mouse of a
second window would land in the first one, and every popup or tooltip that sticks out of a window
would become a toplevel window that the compositor places (or tiles) anywhere.

## Decision

Tiles (ui_shell.cpp `begin_tile`, `dock_space`):
- The schema still covers the whole work area at the back. Over it lies a dockspace host (no
  background) whose central node stays empty (`PassthruCentralNode | NoDockingOverCentralNode`): the
  mouse goes through it to the schema, nothing docks over the schema, and `free_area_` (fit to view,
  the editor's tab bar, code tabs) is the central node. Showing, hiding or resizing a tile still never
  changes the schema's size.
- Tiles: side bar, inspector, each bottom tab (tickets, agents, build & tests, costs, drift — each its
  own window now) and the chat. The default layout is the one of ADR 0021 (side left, inspector
  right, bottom tabs between them; the chat floats), built with DockBuilder from `side_frac`,
  `inspector_frac`, `bottom_frac`; "Windows → Tiles and windows → Reset the layout" builds it again.
- Each tile is docked (dragged by its tab to any edge, tabbed with others), floating inside the main
  window, or a window of the desktop. The mode is chosen by dragging, by the context menu of its tab
  or title bar, by the button in its header, or in "Windows → Tiles and windows". Docking a tile back
  uses its last node, else the node of a tile of the same edge, else a new part split off the
  schema; the chat docks as its own tile under the inspector.
- A tile sent out is a window class with `NoAutoMerge` (it never merges back into the main window)
  and without `NoDecoration`/`NoTaskBarIcon`: the window manager decorates, moves and resizes it and
  lists it like an application; ImGui's title bar is hidden then. All windows carry the application
  id "karkas" (Wayland app_id, X11 class), so desktop rules can name them.
- Docked/floating places are kept by ImGui's ini text in `~/.config/karkas/gui_layout.ini` (saved a
  few seconds after a change and at exit); the tiles sent out are listed in gui.json (`os_windows`).
  Headless checks start from the default layout unless `--layout FILE` is given.

Windows of the desktop on Wayland (viewports.cpp, "virtual positions"):
- Each window gets a virtual position: the main window 0,0, the others where ImGui put them (a new
  one right of every window there is, so none overlaps another in ImGui's space).
  `Platform_GetWindowPos/SetWindowPos` read and keep these; the mouse callback of a secondary window
  adds its virtual position (the backend's `glfwGetWindowPos` adds 0).
- Every window is a monitor of its own (`platform_io.Monitors` rebuilt each frame from the
  viewports): popups and tooltips are clamped into the window that opens them instead of becoming
  windows of their own.
- A floating window (not sent out on purpose) is kept inside the main window
  (`keep_floating_inside`), since leaving it would make a window the compositor places anywhere. A
  tile leaves the main window only through its button or menu, and comes back the same way; dragging
  a window between windows of the desktop cannot work without positions.
- On X11 ImGui's own behaviour stays: a window dragged out becomes a window of the desktop.
- `KARKAS_GUI_WINDOWS=0` turns the windows of the desktop off (docking stays);
  `KARKAS_GUI_VIRTUAL_WINDOWS=1` uses the virtual positions on X11 (checks of the Wayland path).

## Consequences

- The fixed panel layout of ADR 0021 (`begin_panel`, panel sizes as fractions saved on drag) is gone;
  the fractions only build the default layout. Tiles must be submitted after the dockspace host each
  frame (ImGui undocks a window submitted before its host).
- The editor tab bar and fit to view follow the central node, so a floating tile can cover the
  schema (as the floating chat did before).
- Headless checks: GLFW prefers Wayland when `WAYLAND_DISPLAY` is set, even under xvfb-run, and
  would open the windows of the desktop on the user's screen; checks run with `env -u
  WAYLAND_DISPLAY`. `KARKAS_GUI_LIST_WINDOWS=1` prints the windows of the desktop and the docked tiles
  at the end of a headless run. Scripted mouse input says the mouse is over the main window.
- Verified under Xvfb: default layout; the chat docked as a tile under the inspector; the chat sent
  out as a decorated window of the desktop (virtual positions, screen captured: it draws its content);
  a floating window dragged past the edge stays inside; the layout survives a restart (`--layout`).
  Accidentally also on Hyprland itself (the first check ran on the user's Wayland session): the chat
  window was created decorated at its virtual position. Interactive use on Hyprland (tiling of the
  windows sent out, focus, closing them with the compositor) is manual.
