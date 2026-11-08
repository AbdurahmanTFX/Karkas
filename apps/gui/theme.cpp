#include "theme.hpp"

#include <filesystem>
#include <vector>

#include "icons.hpp"

namespace kk::gui::theme {

namespace {
ImFont* g_ui = nullptr;
ImFont* g_heading = nullptr;
ImFont* g_mono = nullptr;
ImFont* g_icons_big = nullptr;

std::string first_existing(const std::vector<std::string>& candidates) {
  for (const auto& c : candidates) {
    std::error_code ec;
    if (!c.empty() && std::filesystem::exists(c, ec)) return c;
  }
  return {};
}
}  // namespace

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImVec4 accent(float alpha) {
  ImVec4 a = col(kAccent);
  a.w = alpha;
  return a;
}

void apply(float scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();
  ImGui::StyleColorsDark(&s);
  s.WindowPadding = ImVec2(10, 8);
  s.FramePadding = ImVec2(8, 4);
  s.CellPadding = ImVec2(6, 3);
  s.ItemSpacing = ImVec2(8, 5);
  s.ItemInnerSpacing = ImVec2(6, 4);
  s.IndentSpacing = 14;
  s.ScrollbarSize = 11;
  s.GrabMinSize = 10;
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 1;
  s.PopupBorderSize = 1;
  s.FrameBorderSize = 0;
  s.TabBorderSize = 0;
  s.TabBarBorderSize = 1;
  s.TabBarOverlineSize = 2;
  s.WindowRounding = 6;
  s.ChildRounding = 4;
  s.FrameRounding = 4;
  s.PopupRounding = 6;
  s.ScrollbarRounding = 8;
  s.GrabRounding = 4;
  s.TabRounding = 4;
  s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  s.SeparatorTextBorderSize = 1;
  s.DockingSeparatorSize = 1;
  s.WindowMenuButtonPosition = ImGuiDir_None;  // no collapse triangle on docked panels
  ImVec4* c = s.Colors;
  auto rgb = [](int r, int g, int b, float a = 1.0f) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
  };
  c[ImGuiCol_Text] = rgb(204, 204, 204);
  c[ImGuiCol_TextDisabled] = rgb(133, 133, 133);
  c[ImGuiCol_WindowBg] = rgb(31, 31, 31);
  c[ImGuiCol_ChildBg] = rgb(31, 31, 31, 0.0f);
  c[ImGuiCol_PopupBg] = rgb(37, 37, 38);
  c[ImGuiCol_Border] = rgb(48, 48, 48);
  c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_FrameBg] = rgb(49, 49, 49);
  c[ImGuiCol_FrameBgHovered] = rgb(60, 60, 60);
  c[ImGuiCol_FrameBgActive] = rgb(64, 64, 64);
  c[ImGuiCol_TitleBg] = rgb(24, 24, 24);
  c[ImGuiCol_TitleBgActive] = rgb(24, 24, 24);
  c[ImGuiCol_TitleBgCollapsed] = rgb(24, 24, 24);
  c[ImGuiCol_MenuBarBg] = rgb(24, 24, 24);
  c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = rgb(80, 80, 80, 0.6f);
  c[ImGuiCol_ScrollbarGrabHovered] = rgb(100, 100, 100, 0.8f);
  c[ImGuiCol_ScrollbarGrabActive] = rgb(120, 120, 120, 0.9f);
  c[ImGuiCol_CheckMark] = col(kAccent);
  c[ImGuiCol_SliderGrab] = col(kAccent);
  c[ImGuiCol_SliderGrabActive] = rgb(30, 140, 230);
  c[ImGuiCol_Button] = rgb(49, 49, 49);
  c[ImGuiCol_ButtonHovered] = rgb(62, 62, 62);
  c[ImGuiCol_ButtonActive] = rgb(0, 95, 170);
  c[ImGuiCol_Header] = rgb(4, 57, 94);          // list selection
  c[ImGuiCol_HeaderHovered] = rgb(42, 45, 46);  // list hover
  c[ImGuiCol_HeaderActive] = rgb(9, 71, 113);
  c[ImGuiCol_Separator] = rgb(48, 48, 48);
  c[ImGuiCol_SeparatorHovered] = col(kAccent);
  c[ImGuiCol_SeparatorActive] = col(kAccent);
  c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_ResizeGripHovered] = accent(0.6f);
  c[ImGuiCol_ResizeGripActive] = accent(0.9f);
  c[ImGuiCol_Tab] = rgb(24, 24, 24);
  c[ImGuiCol_TabHovered] = rgb(31, 31, 31);
  c[ImGuiCol_TabSelected] = rgb(31, 31, 31);
  c[ImGuiCol_TabSelectedOverline] = col(kAccent);
  c[ImGuiCol_TabDimmed] = rgb(24, 24, 24);
  c[ImGuiCol_TabDimmedSelected] = rgb(31, 31, 31);
  c[ImGuiCol_TabDimmedSelectedOverline] = rgb(70, 70, 70);
  c[ImGuiCol_DockingPreview] = accent(0.5f);
  c[ImGuiCol_DockingEmptyBg] = rgb(31, 31, 31);
  c[ImGuiCol_TableHeaderBg] = rgb(37, 37, 38);
  c[ImGuiCol_TableBorderStrong] = rgb(48, 48, 48);
  c[ImGuiCol_TableBorderLight] = rgb(40, 40, 40);
  c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
  c[ImGuiCol_TextSelectedBg] = rgb(38, 79, 120);
  c[ImGuiCol_NavCursor] = col(kAccent);
  c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.45f);
  c[ImGuiCol_TextLink] = rgb(55, 148, 255);
  s.ScaleAllSizes(scale);
}

void load_fonts(float scale, const std::string& configured_ui_font) {
  ImGuiIO& io = ImGui::GetIO();
  float size = 15.0f * scale;
  auto ui = first_existing({configured_ui_font, "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                            "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
  auto symbols = first_existing({"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
  auto medium = first_existing({"/usr/share/fonts/noto/NotoSans-Medium.ttf", "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
                                "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", ui});
  auto mono = first_existing({"/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf", "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
                              "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"});
  const ImWchar* cyr = io.Fonts->GetGlyphRangesCyrillic();
  // Arrows, geometric shapes and dingbats come from DejaVu when the UI font lacks them.
  static const ImWchar extra[] = {0x2000, 0x206F, 0x2190, 0x21FF, 0x2500, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0};
  auto add = [&](const std::string& file, float px, const ImWchar* ranges, bool merge) -> ImFont* {
    if (file.empty()) return nullptr;
    ImFontConfig cfg;
    cfg.MergeMode = merge;
    cfg.OversampleH = 2;
    cfg.PixelSnapH = false;
    return io.Fonts->AddFontFromFileTTF(file.c_str(), px, &cfg, ranges);
  };
  g_ui = add(ui, size, cyr, false);
  if (g_ui == nullptr) g_ui = io.Fonts->AddFontDefault();
  add(symbols, size, extra, true);
  add_icon_glyphs(g_ui, size);
  g_heading = add(medium, 12.5f * scale, cyr, false);
  if (g_heading == nullptr) g_heading = g_ui;
  // Big icons (activity bar): a font with only the icon glyphs, rasterized at that size.
  static const ImWchar space[] = {0x20, 0x20, 0};
  g_icons_big = add(ui, 24.0f * scale, space, false);
  if (g_icons_big != nullptr) add_icon_glyphs(g_icons_big, 24.0f * scale);
  g_mono = add(mono, 14.0f * scale, cyr, false);
  if (g_mono == nullptr) g_mono = g_ui;
  add(symbols, 14.0f * scale, extra, true);
  io.FontDefault = g_ui;
  io.Fonts->Build();
  rasterize_icon_glyphs();
}

ImFont* ui_font() { return g_ui; }
ImFont* heading_font() { return g_heading; }
ImFont* mono_font() { return g_mono; }
ImFont* icon_font_big() { return g_icons_big != nullptr ? g_icons_big : g_ui; }

}  // namespace kk::gui::theme
