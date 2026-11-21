// karkas-gui (14): Dear ImGui (docking) + GLFW + OpenGL 3.3 + imgui-node-editor.
//   karkas-gui [--project <dir>] [--scope <block>] [--select <block>] [--all-levels]
//              [--lang en|ru] [--block-scale F] [--settings] [--chat] [--script "..."] [--drag x1,y1,x2,y2]
//              [--frames N --screenshot out.png]   (headless checks)
// Without a project (started outside one) the window opens with "Open project".
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <spdlog/spdlog.h>

#include <GL/gl.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "drop.hpp"
#include "cursors.hpp"
#include "kk/core/config.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "picker.hpp"
#include "theme.hpp"
#include "png.hpp"
#include "script.hpp"
#include "ui.hpp"
#include "viewports.hpp"
#include "worker.hpp"

namespace fs = std::filesystem;

namespace {

void usage() {
  std::cerr << "usage: karkas-gui [--project <dir>] [--scope <block>] [--select <block>] [--all-levels]\n"
               "                  [--lang en|ru] [--block-scale F] [--settings] [--chat] [--script \"...\"]\n"
               "                  [--drag x1,y1,x2,y2] [--layout file.ini] [--prototype]\n"
               "                  [--frames N] [--screenshot out.png] [--version]\n";
}

void render(GLFWwindow* window) {
  ImGui::Render();
  int w = 0;
  int h = 0;
  glfwGetFramebufferSize(window, &w, &h);
  glViewport(0, 0, w, h);
  glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void screenshot_to(GLFWwindow* window, const std::string& path) {
  int w = 0;
  int h = 0;
  glfwGetFramebufferSize(window, &w, &h);
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  std::vector<std::uint8_t> flipped(px.size());
  auto row = static_cast<std::size_t>(w) * 4;
  for (std::size_t y = 0; y < static_cast<std::size_t>(h); ++y) {
    std::copy_n(px.begin() + static_cast<long>((static_cast<std::size_t>(h) - 1 - y) * row), row,
                flipped.begin() + static_cast<long>(y * row));
  }
  kk::gui::write_png(path, w, h, flipped);
}

}  // namespace

int main(int argc, char** argv) {
  std::string project, scope, select, screenshot, lang, layout_file;
  float block_scale = 0;
  bool all_levels = false;
  bool open_settings = false;
  bool open_chat = false;
  bool open_proto = false;
  std::string script_text;  // headless checks: scripted input (script.hpp)
  int frames = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--project" || a == "-C") project = next();
    else if (a == "--scope") scope = next();
    else if (a == "--select") select = next();
    else if (a == "--all-levels") all_levels = true;
    else if (a == "--lang") lang = next();
    else if (a == "--block-scale") block_scale = std::strtof(next().c_str(), nullptr);
    else if (a == "--settings") open_settings = true;
    else if (a == "--chat") open_chat = true;
    else if (a == "--prototype") open_proto = true;
    else if (a == "--layout") layout_file = next();
    else if (a == "--script") script_text += next() + ";";
    else if (a == "--drag") script_text += "drag " + kk::core::replace_all(next(), ",", " ") + ";";
    else if (a == "--frames") frames = std::atoi(next().c_str());
    else if (a == "--screenshot") screenshot = next();
    else if (a == "--version") {
      std::cout << "karkas-gui " << KARKAS_VERSION << "\n";
      return 0;
    } else {
      usage();
      return 2;
    }
  }
  std::optional<fs::path> root;
  if (auto r = kk::core::find_project_root(project.empty() ? fs::current_path() : fs::path(project))) {
    root = *r;
  } else if (!project.empty()) {
    std::cerr << "karkas-gui: " << r.error().message << "\n";
    return 1;
  }
  std::string script_error;
  auto script = kk::gui::InputScript::parse(script_text, &script_error);
  if (!script) {
    std::cerr << "karkas-gui: " << script_error << "\n";
    return 2;
  }
  std::string font_cfg;
  if (root) {
    if (auto cfg = kk::core::load_config(*root)) font_cfg = cfg->font;
  }
  spdlog::set_level(spdlog::level::warn);
  if (glfwInit() == 0) {
    std::cerr << "karkas-gui: cannot initialize GLFW\n";
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  if (frames > 0) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  // The windows of the tiles sent out (ADR 0032) carry the same application id: one group in
  // the task bar, and window rules of the desktop can name them ("karkas").
  glfwWindowHintString(GLFW_WAYLAND_APP_ID, "karkas");
  glfwWindowHintString(GLFW_X11_CLASS_NAME, "karkas");
  glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "karkas");
  GLFWwindow* window = glfwCreateWindow(1600, 950, "Karkas", nullptr, nullptr);
  if (window == nullptr) {
    std::cerr << "karkas-gui: cannot create an OpenGL 3.3 window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  // Files dropped on the window: images and files for the note being edited (ADR 0041).
  glfwSetDropCallback(window, [](GLFWwindow*, int count, const char** paths) {
    kk::gui::push_dropped(std::vector<std::string>(paths, paths + count));
  });
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Tiles over the schema (ui_shell.cpp, ADR 0021, 0032): docking, windows of the desktop.
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigWindowsResizeFromEdges = true;
  io.ConfigWindowsMoveFromTitleBarOnly = true;  // a drag inside a window selects text or pans, never moves it
  io.IniFilename = nullptr;  // the dock layout is kept in ~/.config/karkas/gui_layout.ini (below)
  float xs = 1.0f;
  float ys = 1.0f;
  glfwGetWindowContentScale(window, &xs, &ys);
  float scale = std::max(1.0f, xs);
  // IDE look (theme.hpp): dark theme, UI/heading/monospace fonts with Cyrillic, icon glyphs.
  kk::gui::theme::apply(scale);
  kk::gui::theme::load_fonts(scale, font_cfg);
  // Settings (~/.config/karkas/gui.json). Command-line values override them for this run
  // only; headless runs (--frames) never write the file.
  auto settings = kk::gui::load_settings();
  bool persist = frames == 0;
  if (!lang.empty()) settings.lang = kk::gui::lang_from_code(lang);
  if (block_scale > 0) settings.block_scale = std::clamp(block_scale, kk::gui::kMinBlockScale, kk::gui::kMaxBlockScale);
  kk::gui::set_lang(settings.lang);
  io.FontGlobalScale = settings.ui_scale;
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330");
  kk::gui::viewports::init(window, true);
  // The dock layout of the tiles from the last run; headless checks always start from the
  // default layout and never write it.
  // --layout FILE: this file instead (also in headless checks, which otherwise use neither).
  fs::path layout = !layout_file.empty() ? fs::path(layout_file) : persist ? kk::gui::layout_path() : fs::path();
  if (!layout.empty()) {
    if (auto ini = kk::core::read_file(layout)) {
      auto clean = kk::gui::clean_layout_ini(*ini);
      ImGui::LoadIniSettingsFromMemory(clean.data(), clean.size());
    }
  }
  auto save_layout = [&](bool now) {
    if (!io.WantSaveIniSettings && !now) return;
    io.WantSaveIniSettings = false;
    if (layout.empty()) return;
    std::size_t n = 0;
    const char* text = ImGui::SaveIniSettingsToMemory(&n);
    (void)kk::core::write_file(layout, kk::gui::clean_layout_ini(std::string_view(text, n)));
  };
  // Mouse cursors of the desktop's theme (cursors.hpp): the GLFW ones are the old X11 set when
  // the cursor library cannot resolve the theme (its hand points to the upper right).
  struct ThemeCursors {
    GLFWcursor* c[ImGuiMouseCursor_COUNT] = {};
    ImGuiMouseCursor last = -2;
    int windows = 1;
    bool any = false;
  } cursors;
  {
    int size = 24;
    if (const char* s = std::getenv("XCURSOR_SIZE"); s != nullptr && std::atoi(s) > 0) size = std::clamp(std::atoi(s), 8, 256);
    auto make = [&](ImGuiMouseCursor i, std::vector<std::string> names, int standard) {
      if (auto img = kk::gui::load_theme_cursor(names, size)) {
        GLFWimage gi{img->width, img->height, img->rgba.data()};
        cursors.c[i] = glfwCreateCursor(&gi, img->xhot, img->yhot);
        cursors.any = cursors.any || cursors.c[i] != nullptr;
      }
      if (cursors.c[i] == nullptr && standard != 0) cursors.c[i] = glfwCreateStandardCursor(standard);
    };
    make(ImGuiMouseCursor_Arrow, {"default", "left_ptr", "arrow"}, GLFW_ARROW_CURSOR);
    make(ImGuiMouseCursor_TextInput, {"text", "xterm", "ibeam"}, GLFW_IBEAM_CURSOR);
    make(ImGuiMouseCursor_ResizeAll, {"all-scroll", "move", "fleur", "size_all"}, GLFW_RESIZE_ALL_CURSOR);
    make(ImGuiMouseCursor_ResizeNS, {"ns-resize", "row-resize", "sb_v_double_arrow", "size_ver", "v_double_arrow"}, GLFW_RESIZE_NS_CURSOR);
    make(ImGuiMouseCursor_ResizeEW, {"ew-resize", "col-resize", "sb_h_double_arrow", "size_hor", "h_double_arrow"}, GLFW_RESIZE_EW_CURSOR);
    make(ImGuiMouseCursor_ResizeNESW, {"nesw-resize", "fd_double_arrow", "size_bdiag"}, GLFW_RESIZE_NESW_CURSOR);
    make(ImGuiMouseCursor_ResizeNWSE, {"nwse-resize", "bd_double_arrow", "size_fdiag"}, GLFW_RESIZE_NWSE_CURSOR);
    make(ImGuiMouseCursor_Hand, {"pointer", "hand2", "pointing_hand", "hand1", "hand"}, GLFW_POINTING_HAND_CURSOR);
    make(ImGuiMouseCursor_Wait, {"wait", "watch"}, 0);
    make(ImGuiMouseCursor_Progress, {"progress", "left_ptr_watch", "half-busy"}, 0);
    make(ImGuiMouseCursor_NotAllowed, {"not-allowed", "crossed_circle", "forbidden", "no-drop"}, GLFW_NOT_ALLOWED_CURSOR);
    // The theme's cursors replace the backend's (which would set its own every frame).
    if (cursors.any) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  }
  auto apply_cursor = [&] {
    if (!cursors.any) return;
    ImGuiMouseCursor cur = ImGui::GetMouseCursor();
    if (io.MouseDrawCursor || cur == ImGuiMouseCursor_None) {
      if (cursors.last != -1) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
      cursors.last = -1;
      return;
    }
    int windows = ImGui::GetPlatformIO().Viewports.Size;
    if (cur == cursors.last && windows == cursors.windows) return;
    if (cursors.last == -1) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    GLFWcursor* c = cur >= 0 && cur < ImGuiMouseCursor_COUNT ? cursors.c[cur] : nullptr;
    // Every window of the desktop (the tiles sent out) shows the same cursor.
    kk::gui::viewports::set_cursor_everywhere(c != nullptr ? c : cursors.c[ImGuiMouseCursor_Arrow]);
    glfwSetCursor(window, c != nullptr ? c : cursors.c[ImGuiMouseCursor_Arrow]);
    cursors.last = cur;
    cursors.windows = windows;
  };

  // Redraw on events only (14.8): near 0% CPU while idle; ~20 fps while agents work
  // (pulsing blocks). One extra frame after every event lets ImGui settle hovers/popups.
  int settle = 3;
  auto wait = [&](bool animate) {
    if (frames > 0 || animate) {
      glfwWaitEventsTimeout(0.05);
    } else if (settle > 0) {
      --settle;
      glfwPollEvents();
    } else {
      glfwWaitEvents();
      settle = 2;
    }
  };

  bool quit = false;
  bool first_project = true;  // command-line --scope/--select/... apply to the first project only
  while (!quit && glfwWindowShouldClose(window) == 0) {
    if (!root) {
      // No project yet: the window is the "Open project" picker.
      kk::gui::ProjectPicker picker(fs::current_path(), settings.recent);
      int picker_frames = 0;
      while (!root && glfwWindowShouldClose(window) == 0) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        kk::gui::viewports::before_new_frame();
        if (frames > 0 && picker_frames >= frames) script->step(io);
        ImGui::NewFrame();
        kk::gui::viewports::keep_floating_inside();
        bool open = true;
        if (auto chosen = picker.draw(&open, true)) root = *chosen;
        render(window);
        kk::gui::viewports::render_other_windows(window);
        apply_cursor();
        // Headless check of the welcome screen: capture once the script is done.
        if (frames > 0 && ++picker_frames >= frames && script->done() && !root) {
          if (!screenshot.empty()) screenshot_to(window, screenshot);
          quit = true;
          break;
        }
        glfwSwapBuffers(window);
        wait(false);
      }
      continue;
    }
    auto cfg = kk::core::load_config(*root);
    if (!cfg) {
      std::cerr << "karkas-gui: " << cfg.error().str() << "\n";
      if (frames > 0) return 1;
      root.reset();
      continue;
    }
    kk::gui::add_recent(settings, *root);
    if (persist) kk::gui::save_settings(settings);
    glfwSetWindowTitle(window, ("Karkas — " + root->filename().string()).c_str());
    std::optional<fs::path> next_project;
    {
      kk::gui::Worker worker(*root, *cfg, [] { glfwPostEmptyEvent(); });
      kk::gui::Ui ui(worker, settings, persist, [] { glfwPostEmptyEvent(); });
      ui.set_project_root(*root);
      if (first_project) {
        if (open_settings) ui.open_settings();
        if (open_chat) ui.open_chat();
        ui.request().scope = scope == "root" ? "" : scope;
        // Nothing selected: the block of the level shown (the project at the top).
        ui.request().selected = select.empty() ? ui.request().scope : select;
        ui.request().all_levels = all_levels;
        if (open_proto) ui.open_prototype();
      }
      bool first = true;
      int frame = 0;
      int ready_since = -1;
      int phase = 0;  // headless: 0 wait for the level, 1 run the script, 2 let the model follow, 3 done
      int settle_frames = 0;
      auto start = std::chrono::steady_clock::now();
      // KARKAS_GUI_PROFILE=1: CPU time of a frame (UI + render submit), every 60 frames.
      const bool profile = std::getenv("KARKAS_GUI_PROFILE") != nullptr;
      double prof_sum = 0;
      double prof_max = 0;
      double prof_draw = 0;
      int prof_n = 0;
      while (glfwWindowShouldClose(window) == 0) {
        auto model = worker.model();
        auto t0 = std::chrono::steady_clock::now();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        kk::gui::viewports::before_new_frame();
        if (phase == 1) script->step(io);
        ImGui::NewFrame();
        kk::gui::viewports::keep_floating_inside();
        ui.request().char_w = ImGui::CalcTextSize("abcdefghijklmnopqrstuvwxyz").x / 26.0f;
        ui.request().line_h = ImGui::GetTextLineHeightWithSpacing();
        bool animate = ui.draw(model, first);
        double draw_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        first = false;
        if (ui.quit_requested()) glfwSetWindowShouldClose(window, GLFW_TRUE);
        worker.set_request(ui.request());
        render(window);
        kk::gui::viewports::render_other_windows(window);
        apply_cursor();
        save_layout(false);
        if (profile) {
          double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          prof_sum += ms;
          prof_max = std::max(prof_max, ms);
          prof_draw += draw_ms;
          if (++prof_n == 60) {
            std::cerr << "karkas-gui: frame cpu avg " << prof_sum / prof_n << " ms (ui " << prof_draw / prof_n << " ms), max " << prof_max
                      << " ms\n";
            prof_sum = prof_max = prof_draw = 0;
            prof_n = 0;
          }
        }
        ++frame;
        if (auto req = ui.take_open_request(); req && *req != *root) {
          next_project = *req;
          break;
        }
        if (frames > 0) {
          // Headless check: the model of the requested level has been on screen for a few
          // frames (positions applied, then "fit to view"), then the script, then capture.
          bool have = model && model->snap && model->req.scope == ui.request().scope;
          if (!have) ready_since = -1;
          else if (ready_since < 0) ready_since = frame;
          bool ready = have && frame - ready_since >= 6;
          if (phase == 0 && ready && frame >= frames) phase = script->empty() ? 3 : 1;
          if (phase == 1 && script->done()) phase = 2;
          if (phase == 2) {
            bool synced = model && model->req == ui.request();
            settle_frames = synced ? settle_frames + 1 : 0;
            if (settle_frames >= 8) phase = 3;
          }
          if (phase == 3 || std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) {
            if (!screenshot.empty()) screenshot_to(window, screenshot);
            save_layout(true);
            // Headless check of the tiles (ADR 0032): the windows of the desktop and the tiles docked.
            if (std::getenv("KARKAS_GUI_LIST_WINDOWS") != nullptr) {
              for (ImGuiViewport* v : ImGui::GetPlatformIO().Viewports) {
                auto* vp = static_cast<ImGuiViewportP*>(v);
                std::cerr << "window " << (vp->Window != nullptr ? vp->Window->Name : "(main)") << " at " << vp->Pos.x << "," << vp->Pos.y
                          << " size " << vp->Size.x << "x" << vp->Size.y << ((vp->Flags & ImGuiViewportFlags_NoDecoration) ? "" : " decorated")
                          << "\n";
              }
              for (ImGuiWindow* w : GImGui->Windows) {
                if (w->WasActive && w->DockIsActive && w->DockNode != nullptr) std::cerr << "docked " << w->Name << " in " << w->DockNode->ID << "\n";
              }
            }
            quit = true;
            break;
          }
        }
        glfwSwapBuffers(window);
        wait(animate);
      }
    }
    first_project = false;
    if (next_project) {
      root = *next_project;
    } else {
      quit = true;
    }
  }
  if (frames == 0) save_layout(true);  // changes of the last seconds (ImGui saves a few seconds after a change)
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  for (auto* c : cursors.c) {
    if (c != nullptr) glfwDestroyCursor(c);
  }
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
