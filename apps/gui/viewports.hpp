#pragma once
// Tiles as separate windows of the desktop (docs/adr/0032): Dear ImGui's multi-viewports over
// GLFW, made to work where the platform does not tell an application where its windows are.
//
// On Wayland a client never learns the global position of its windows (glfwGetWindowPos
// returns 0, glfwSetWindowPos is refused), while ImGui keeps every viewport in one desktop
// coordinate space. Here each window of the desktop gets a virtual position (the main window
// at 0,0, the others where ImGui put them), the mouse of a window is reported in that space,
// and every window is its own "monitor", so popups and tooltips stay inside the window that
// opened them instead of becoming windows of their own. The compositor places and tiles the
// windows; ImGui only follows their sizes.
#include <imgui.h>

struct GLFWwindow;

namespace kk::gui::viewports {

// Turns docking and multi-viewports on (`enable_windows` false: docking only) and, on
// Wayland, installs the virtual positions. Call after ImGui_ImplGlfw_InitForOpenGL.
void init(GLFWwindow* main, bool enable_windows);
// True when windows of the desktop work (multi-viewports on).
[[nodiscard]] bool enabled();
// True on Wayland: windows cannot be dragged out of the main window, only sent out with the
// tile's button; a floating window stays inside the main window.
[[nodiscard]] bool virtual_positions();
// Call between ImGui_ImplGlfw_NewFrame and ImGui::NewFrame.
void before_new_frame();
// Call after rendering the main window: updates, renders and swaps the other windows.
void render_other_windows(GLFWwindow* main);
// Keeps every floating window (not a tile sent out on purpose) inside the main window
// (virtual positions only: a window that leaves it would become a window of the desktop that
// the compositor places anywhere); call after ImGui::NewFrame.
void keep_floating_inside();
// A free spot for a new window of the desktop (virtual positions: right of every window
// there is; else near the main window's right edge).
[[nodiscard]] ImVec2 spot_for_new_window(ImVec2 size);
// Sets the mouse cursor on every window of the desktop.
void set_cursor_everywhere(void* glfw_cursor);

}  // namespace kk::gui::viewports
