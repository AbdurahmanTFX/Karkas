// karkas-gui (14): Dear ImGui (docking) + GLFW + OpenGL 3.3 + imgui-node-editor.
//   karkas-gui [--project <dir>] [--scope <block>] [--select <block>] [--all-levels]
//              [--lang en|ru] [--block-scale F] [--settings] [--drag x1,y1,x2,y2]
//              [--frames N --screenshot out.png]   (headless checks)
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <spdlog/spdlog.h>

#include <GL/gl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "kk/core/config.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "png.hpp"
#include "ui.hpp"
#include "worker.hpp"

namespace fs = std::filesystem;

namespace {

std::string find_font(const std::string& configured, bool mono) {
  std::vector<std::string> candidates;
  if (!configured.empty() && !mono) candidates.push_back(configured);
  if (mono) {
    candidates = {"/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
                  "/usr/share/fonts/liberation/LiberationMono-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"};
  } else {
    for (const char* f : {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                          "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
      candidates.emplace_back(f);
    }
  }
  for (const auto& c : candidates) {
    if (fs::exists(c)) return c;
  }
  return {};
}

void usage() {
  std::cerr << "usage: karkas-gui [--project <dir>] [--scope <block>] [--select <block>] [--all-levels]\n"
               "                  [--lang en|ru] [--block-scale F] [--settings] [--drag x1,y1,x2,y2]\n"
               "                  [--frames N] [--screenshot out.png] [--version]\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string project, scope, select, screenshot, lang;
  float block_scale = 0;
  bool all_levels = false;
  bool open_settings = false;
  std::vector<float> drag;  // headless check: a mouse drag in window pixels
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
    else if (a == "--drag") {
      for (const auto& v : kk::core::split(next(), ',')) drag.push_back(std::strtof(v.c_str(), nullptr));
      if (drag.size() != 4) {
        usage();
        return 2;
      }
    }
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
  auto root = kk::core::find_project_root(project.empty() ? fs::current_path() : fs::path(project));
  if (!root) {
    std::cerr << "karkas-gui: " << root.error().message << "\n";
    return 1;
  }
  auto cfg = kk::core::load_config(*root);
  if (!cfg) {
    std::cerr << "karkas-gui: " << cfg.error().str() << "\n";
    return 1;
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
  GLFWwindow* window = glfwCreateWindow(1600, 950, ("Karkas — " + root->filename().string()).c_str(), nullptr, nullptr);
  if (window == nullptr) {
    std::cerr << "karkas-gui: cannot create an OpenGL 3.3 window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;  // the panel layout is rebuilt each start (window ids are language independent)
  ImGui::StyleColorsDark();
  float xs = 1.0f;
  float ys = 1.0f;
  glfwGetWindowContentScale(window, &xs, &ys);
  float scale = std::max(1.0f, xs);
  ImGui::GetStyle().ScaleAllSizes(scale);
  // Cyrillic glyphs (14.8): ImGui 1.91 needs explicit glyph ranges.
  auto font = find_font(cfg->font, false);
  if (!font.empty()) {
    io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f * scale, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    static const ImWchar extra[] = {0x2000, 0x206F, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
    ImFontConfig merge;
    merge.MergeMode = true;
    io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f * scale, &merge, extra);
  } else {
    io.Fonts->AddFontDefault();
  }
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

  kk::gui::Worker worker(*root, *cfg, [] { glfwPostEmptyEvent(); });
  kk::gui::Ui ui(worker, settings, persist);
  if (open_settings) ui.open_settings();
  ui.request().scope = scope;
  ui.request().selected = select;
  ui.request().all_levels = all_levels;
  bool first = true;
  int frame = 0;
  int settle = 3;
  std::shared_ptr<const kk::gui::Model> model;
  int ready_since = -1;
  int drag_frame = -1;  // >= 0 while the scripted drag runs
  std::uint64_t release_serial = 0;
  auto start = std::chrono::steady_clock::now();
  while (glfwWindowShouldClose(window) == 0) {
    model = worker.model();
    bool animate = false;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    if (drag_frame >= 0 && drag_frame <= 12) {
      // Scripted drag (headless checks): move, press, 10 steps, release.
      if (drag_frame == 0) io.AddMousePosEvent(drag[0], drag[1]);
      if (drag_frame == 1) io.AddMouseButtonEvent(0, true);
      if (drag_frame >= 2 && drag_frame <= 11) {
        float t = static_cast<float>(drag_frame - 1) / 10.0f;
        io.AddMousePosEvent(drag[0] + (drag[2] - drag[0]) * t, drag[1] + (drag[3] - drag[1]) * t);
      }
      if (drag_frame == 12) io.AddMouseButtonEvent(0, false);
    }
    ImGui::NewFrame();
    ui.request().char_w = ImGui::CalcTextSize("abcdefghijklmnopqrstuvwxyz").x / 26.0f;
    ui.request().line_h = ImGui::GetTextLineHeightWithSpacing();
    animate = ui.draw(model, first);
    first = false;
    if (ui.quit_requested()) glfwSetWindowShouldClose(window, GLFW_TRUE);
    worker.set_request(ui.request());
    ImGui::Render();
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ++frame;
    if (frames > 0) {
      // Headless check: wait until the worker delivered a model, then capture.
      // Ready: the model of the requested level has been on screen for a few frames
      // (positions applied, then "fit to view").
      bool have = model && model->snap && model->req.scope == ui.request().scope;
      if (!have) ready_since = -1;
      else if (ready_since < 0) ready_since = frame;
      bool ready = have && frame - ready_since >= 6;
      auto waited = std::chrono::steady_clock::now() - start;
      if (!drag.empty() && ready && frame >= frames) {
        if (drag_frame == 12) release_serial = model->serial;
        if (drag_frame < 12 || model->serial <= release_serial || drag_frame < 30) {
          ++drag_frame;
          ready = false;
        }
      }
      if ((frame >= frames && ready) || waited > std::chrono::seconds(30)) {
        if (!screenshot.empty()) {
          std::vector<std::uint8_t> px(static_cast<std::size_t>(w * h * 4));
          glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
          std::vector<std::uint8_t> flipped(px.size());
          for (int y = 0; y < h; ++y) {
            std::copy_n(px.begin() + static_cast<long>((h - 1 - y) * w * 4), w * 4, flipped.begin() + static_cast<long>(y * w * 4));
          }
          kk::gui::write_png(screenshot, w, h, flipped);
        }
        break;
      }
      glfwSwapBuffers(window);
      glfwWaitEventsTimeout(0.05);
      continue;
    }
    glfwSwapBuffers(window);
    // Redraw on events only (14.8): near 0% CPU while idle; ~20 fps while agents work
    // (pulsing blocks). One extra frame after every event lets ImGui settle hovers/popups.
    if (animate) {
      glfwWaitEventsTimeout(0.05);
    } else if (settle > 0) {
      --settle;
      glfwPollEvents();
    } else {
      glfwWaitEvents();
      settle = 2;
    }
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
