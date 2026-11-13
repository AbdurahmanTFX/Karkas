#pragma once
// Text the mouse selects like in a browser (ADR 0021): press and drag — over several pieces of
// one region too —, a double click selects a word, a triple click the whole piece, Shift+click
// extends; Ctrl+C or the context menu copies, Ctrl+A selects everything. A region (the chat
// dialog, the live feed) draws its texts through one TextSelect between begin() and end() every
// frame; the selection is kept as (piece id, byte offset) of its two ends.
#include <imgui.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kk::gui::ui {

// From byte `at` on, the text has `color` (0: the color of the piece); sorted by `at`.
struct TextSpan {
  std::size_t at = 0;
  ImU32 color = 0;
};

// Lines of `text` wrapped at `width` (ImGui's word wrap): [begin, end) byte offsets. A text that
// ends with a line break has no extra empty line (as ImGui draws it).
std::vector<std::pair<std::size_t, std::size_t>> wrap_lines(std::string_view text, ImFont* font, float size, float width);

// The word around byte `off` (letters, digits, '_', and '.', '-', '/', ':' inside a word, so a
// block id, a ticket or a path is one word).
std::pair<std::size_t, std::size_t> word_at(std::string_view text, std::size_t off);

// KSL colors as ksl_wrapped draws them: directives, records, trail levels, comments, sections.
std::vector<TextSpan> ksl_spans(std::string_view text);

class TextSelect {
 public:
  void begin();
  // A wrapped text at the cursor: as wide as the text wrap position, else the rest of the line.
  void text(const char* id, std::string_view text, ImVec4 color, ImFont* font = nullptr, const std::vector<TextSpan>* spans = nullptr);
  void end();
  void clear();
  [[nodiscard]] bool has_selection() const {
    return anchor_.piece != 0 && (anchor_.piece != focus_.piece || anchor_.off != focus_.off);
  }
  [[nodiscard]] const std::string& selected() const { return selected_; }  // as of the last frame

 private:
  struct Pos {
    ImGuiID piece = 0;
    std::size_t off = 0;
  };
  struct Layout {
    std::size_t hash = 0;
    float width = 0, size = 0;
    ImFont* font = nullptr;
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    float max_w = 0;
    int used = 0;
  };
  const Layout& layout(ImGuiID id, std::string_view text, ImFont* font, float size, float width);

  Pos anchor_, focus_;
  bool dragging_ = false;
  // This frame.
  bool inside_ = false;  // between the two ends while the pieces are drawn
  int ends_seen_ = 0;
  int parts_n_ = 0;
  std::string parts_;  // selected text of the pieces drawn so far
  bool pressed_on_text_ = false;
  bool have_target_ = false;
  Pos target_;  // where a drag is now
  ImGuiID first_piece_ = 0, last_piece_ = 0;
  std::size_t last_size_ = 0;
  // Across frames.
  std::string selected_;
  std::string menu_piece_;  // the piece right clicked ("copy the whole message")
  bool open_menu_ = false;
  int frame_ = 0;
  std::unordered_map<ImGuiID, Layout> cache_;
};

}  // namespace kk::gui::ui
