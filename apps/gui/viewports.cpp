#include "viewports.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace kk::gui::viewports {

namespace {

bool g_enabled = false;
bool g_virtual = false;
float g_dpi = 1.0f;
std::unordered_map<ImGuiID, ImVec2> g_pos;  // virtual position of each window of the desktop
void (*g_create)(ImGuiViewport*) = nullptr;
void (*g_destroy)(ImGuiViewport*) = nullptr;
void (*g_set_title)(ImGuiViewport*, const char*) = nullptr;

// The mouse of a window of the desktop in the one space of ImGui: its virtual position plus
// the position inside it (the backend adds glfwGetWindowPos, which is 0 on Wayland).
void cursor_pos(GLFWwindow* w, double x, double y) {
  if (ImGuiViewport* vp = ImGui::FindViewportByPlatformHandle(w); vp != nullptr && vp != ImGui::GetMainViewport()) {
    x += vp->Pos.x;
    y += vp->Pos.y;
  }
  ImGui_ImplGlfw_CursorPosCallback(w, x, y);
}

void create_window(ImGuiViewport* vp) {
  g_create(vp);
  g_pos[vp->ID] = vp->Pos;
  if (auto* w = static_cast<GLFWwindow*>(vp->PlatformHandle); w != nullptr) glfwSetCursorPosCallback(w, cursor_pos);
}

void destroy_window(ImGuiViewport* vp) {
  g_pos.erase(vp->ID);
  g_destroy(vp);
}

ImVec2 get_pos(ImGuiViewport* vp) {
  if (vp == ImGui::GetMainViewport()) return {0, 0};
  auto it = g_pos.find(vp->ID);
  return it != g_pos.end() ? it->second : vp->Pos;
}

void set_pos(ImGuiViewport* vp, ImVec2 pos) { g_pos[vp->ID] = pos; }

// The title of a window of the desktop: the tile's name without its icon glyph (private use
// area U+E000–U+F8FF, which window managers show as a box), after "Karkas · ".
void set_title(ImGuiViewport* vp, const char* title) {
  std::string t;
  const auto* s = reinterpret_cast<const unsigned char*>(title);
  for (std::size_t i = 0; s[i] != 0;) {
    bool pua = s[i] == 0xEE || (s[i] == 0xEF && s[i + 1] >= 0x80 && s[i + 1] <= 0xA3);
    int len = s[i] < 0x80 ? 1 : (s[i] >> 5) == 0x6 ? 2 : (s[i] >> 4) == 0xE ? 3 : 4;
    if (!pua) t.append(reinterpret_cast<const char*>(s + i), static_cast<std::size_t>(len));
    for (int k = 0; k < len && s[i] != 0; ++k) ++i;
  }
  auto first = t.find_first_not_of(' ');
  t = first == std::string::npos ? std::string() : t.substr(first);
  g_set_title(vp, ("Karkas · " + t).c_str());
}

}  // namespace

void init(GLFWwindow* main, bool enable_windows) {
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigDockingWithShift = false;
  io.ConfigDockingTransparentPayload = true;
  // KARKAS_GUI_WINDOWS=0 turns the windows of the desktop off (docking stays);
  // KARKAS_GUI_VIRTUAL_WINDOWS=1 uses the virtual positions on X11 too (tests of the Wayland path).
  if (const char* e = std::getenv("KARKAS_GUI_WINDOWS"); e != nullptr && std::string(e) == "0") enable_windows = false;
  if ((io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) == 0) enable_windows = false;
  g_enabled = enable_windows;
  if (!enable_windows) return;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  io.ConfigViewportsNoAutoMerge = false;
  io.ConfigViewportsNoTaskBarIcon = false;
  float xs = 1.0f;
  float ys = 1.0f;
  glfwGetWindowContentScale(main, &xs, &ys);
  g_dpi = std::max(1.0f, xs);
  ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
  g_set_title = pio.Platform_SetWindowTitle;
  pio.Platform_SetWindowTitle = set_title;
  const char* force = std::getenv("KARKAS_GUI_VIRTUAL_WINDOWS");
  g_virtual = glfwGetPlatform() == GLFW_PLATFORM_WAYLAND || (force != nullptr && std::string(force) == "1");
  if (!g_virtual) return;
  g_create = pio.Platform_CreateWindow;
  g_destroy = pio.Platform_DestroyWindow;
  pio.Platform_CreateWindow = create_window;
  pio.Platform_DestroyWindow = destroy_window;
  pio.Platform_GetWindowPos = get_pos;
  pio.Platform_SetWindowPos = set_pos;
}

bool enabled() { return g_enabled; }
bool virtual_positions() { return g_enabled && g_virtual; }

void before_new_frame() {
  if (!virtual_positions()) return;
  // Every window of the desktop is a monitor of its own: popups and tooltips are kept inside
  // the window that opens them (else they would be windows of their own, placed by the
  // compositor anywhere), and a window of the desktop is never clamped onto another one.
  ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
  const ImGuiIO& io = ImGui::GetIO();
  pio.Monitors.resize(0);
  for (ImGuiViewport* vp : pio.Viewports) {
    ImGuiPlatformMonitor m;
    bool main = vp == ImGui::GetMainViewport();
    m.MainPos = m.WorkPos = main ? ImVec2(0, 0) : vp->Pos;
    m.MainSize = m.WorkSize = main ? io.DisplaySize : vp->Size;
    if (m.MainSize.x <= 0 || m.MainSize.y <= 0) continue;
    m.DpiScale = g_dpi;
    pio.Monitors.push_back(m);
  }
  if (pio.Monitors.empty()) {
    ImGuiPlatformMonitor m;
    m.MainSize = m.WorkSize = ImVec2(std::max(1.0f, io.DisplaySize.x), std::max(1.0f, io.DisplaySize.y));
    m.DpiScale = g_dpi;
    pio.Monitors.push_back(m);
  }
}

void render_other_windows(GLFWwindow* main) {
  if (!g_enabled) return;
  ImGui::UpdatePlatformWindows();
  ImGui::RenderPlatformWindowsDefault();
  glfwMakeContextCurrent(main);
}

void keep_floating_inside() {
  if (!virtual_positions()) return;
  ImGuiContext& g = *GImGui;
  ImGuiViewport* main = ImGui::GetMainViewport();
  ImVec2 lo = main->Pos;
  ImVec2 hi(main->Pos.x + main->Size.x, main->Pos.y + main->Size.y);
  if (hi.x - lo.x < 50 || hi.y - lo.y < 50) return;
  for (ImGuiWindow* w : g.Windows) {
    if (!w->WasActive || w->RootWindowDockTree != w || w->DockIsActive) continue;
    if ((w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu)) != 0) continue;
    if ((w->WindowClass.ViewportFlagsOverrideSet & ImGuiViewportFlags_NoAutoMerge) != 0) continue;  // a tile sent out on purpose
    ImVec2 size(std::min(w->Size.x, hi.x - lo.x), std::min(w->Size.y, hi.y - lo.y));
    ImVec2 pos(std::clamp(w->Pos.x, lo.x, hi.x - size.x), std::clamp(w->Pos.y, lo.y, hi.y - size.y));
    if (size.x != w->Size.x || size.y != w->Size.y) ImGui::SetWindowSize(w, size, ImGuiCond_Always);
    if (pos.x != w->Pos.x || pos.y != w->Pos.y) ImGui::SetWindowPos(w, pos, ImGuiCond_Always);
  }
}

ImVec2 spot_for_new_window(ImVec2 size) {
  ImGuiViewport* main = ImGui::GetMainViewport();
  if (!virtual_positions()) {
    // Over the right part of the main window: the window manager may move it anyway.
    return {main->Pos.x + std::max(0.0f, main->Size.x - size.x - 40), main->Pos.y + 80};
  }
  // Virtual positions: right of every window there is, so no two windows overlap in ImGui's space.
  float x = main->Pos.x + main->Size.x + 200;
  for (ImGuiViewport* vp : ImGui::GetPlatformIO().Viewports) x = std::max(x, vp->Pos.x + vp->Size.x + 200);
  return {x, main->Pos.y};
}

void set_cursor_everywhere(void* cursor) {
  for (ImGuiViewport* vp : ImGui::GetPlatformIO().Viewports) {
    if (auto* w = static_cast<GLFWwindow*>(vp->PlatformHandle); w != nullptr) glfwSetCursor(w, static_cast<GLFWcursor*>(cursor));
  }
}

}  // namespace kk::gui::viewports
