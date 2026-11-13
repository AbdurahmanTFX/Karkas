#include "selectable.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <functional>

#include "i18n.hpp"
#include "icons.hpp"
#include "theme.hpp"

namespace kk::gui::ui {

namespace {

// The start of the character at or before byte `off`.
std::size_t char_start(std::string_view s, std::size_t off) {
  off = std::min(off, s.size());
  while (off > 0 && off < s.size() && (static_cast<unsigned char>(s[off]) & 0xC0) == 0x80) --off;
  return off;
}

float width_of(ImFont* font, float size, std::string_view s, std::size_t b, std::size_t e) {
  if (e <= b) return 0.0f;
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.data() + b, s.data() + e).x;
}

// The offset in the line [b, e) nearest to `x` (from the line start).
std::size_t offset_at_x(ImFont* font, float scale, std::string_view s, std::size_t b, std::size_t e, float x) {
  float acc = 0;
  std::size_t p = b;
  while (p < e) {
    unsigned int c = 0;
    int n = ImTextCharFromUtf8(&c, s.data() + p, s.data() + e);
    if (n <= 0) n = 1;
    float w = font->GetCharAdvance(static_cast<ImWchar>(c)) * scale;
    if (acc + w * 0.5f > x) return p;
    acc += w;
    p += static_cast<std::size_t>(n);
  }
  return e;
}

bool word_byte(unsigned char c) { return c >= 0x80 || std::isalnum(c) != 0 || c == '_'; }
bool joiner(unsigned char c) { return c == '.' || c == '-' || c == '/' || c == ':'; }

}  // namespace

std::vector<std::pair<std::size_t, std::size_t>> wrap_lines(std::string_view text, ImFont* font, float size, float width) {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  float scale = size / font->FontSize;
  const char* base = text.data();
  std::size_t pos = 0;
  while (true) {
    std::size_t nl = text.find('\n', pos);
    std::size_t le = nl == std::string_view::npos ? text.size() : nl;
    if (le == pos) {
      if (nl != std::string_view::npos || out.empty()) out.emplace_back(pos, pos);
    } else {
      std::size_t p = pos;
      while (p < le) {
        const char* w = font->CalcWordWrapPositionA(scale, base + p, base + le, width);
        auto we = static_cast<std::size_t>(w - base);
        if (we <= p) we = p + static_cast<std::size_t>(std::max(1, ImTextCountUtf8BytesFromChar(base + p, base + le)));
        we = std::min(we, le);
        out.emplace_back(p, we);
        p = we;
        while (p < le && (text[p] == ' ' || text[p] == '\t')) ++p;  // a wrapped line starts at its word
      }
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
    if (pos == text.size()) break;  // a final line break adds no empty line
  }
  return out;
}

std::pair<std::size_t, std::size_t> word_at(std::string_view s, std::size_t off) {
  off = char_start(s, off);
  if (s.empty()) return {0, 0};
  if (off >= s.size()) off = s.size() - 1;
  auto in_word = [&](std::size_t i) {
    auto c = static_cast<unsigned char>(s[i]);
    if (word_byte(c)) return true;
    // A joiner between two word characters belongs to the word (rules.round, T-0001, a/b.cpp).
    return joiner(c) && i > 0 && i + 1 < s.size() && word_byte(static_cast<unsigned char>(s[i - 1])) &&
           word_byte(static_cast<unsigned char>(s[i + 1]));
  };
  if (!in_word(off)) {
    // Not on a word: the character itself.
    std::size_t e = off + static_cast<std::size_t>(std::max(1, ImTextCountUtf8BytesFromChar(s.data() + off, s.data() + s.size())));
    return {off, std::min(e, s.size())};
  }
  std::size_t b = off;
  while (b > 0 && in_word(b - 1)) --b;
  std::size_t e = off;
  while (e < s.size() && in_word(e)) ++e;
  return {char_start(s, b), e};
}

std::vector<TextSpan> ksl_spans(std::string_view text) {
  const ImU32 comment = IM_COL32(106, 153, 85, 255);
  const ImU32 record = IM_COL32(197, 134, 192, 255);
  const ImU32 directive = IM_COL32(86, 156, 214, 255);
  const ImU32 level = IM_COL32(220, 180, 100, 255);
  const ImU32 section = theme::kTextMuted;
  std::vector<TextSpan> out;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t nl = text.find('\n', pos);
    std::size_t le = nl == std::string_view::npos ? text.size() : nl;
    std::string_view line = text.substr(pos, le - pos);
    if (!line.empty()) {
      if (line.starts_with("--- ")) {
        out.push_back({pos, section});
      } else if (line[0] == '#') {
        out.push_back({pos, comment});
      } else {
        // The head: the directive (KSL aligns values at column 8) or "L2 worker" of a trail line.
        std::size_t end = line.find(' ');
        bool is_level = line.size() > 1 && line[0] == 'L' && line[1] >= '0' && line[1] <= '9';
        if (is_level && end != std::string_view::npos) {
          auto next = line.find_first_not_of(' ', end);
          end = next == std::string_view::npos ? std::string_view::npos : line.find(' ', next);
        }
        if (end == std::string_view::npos) end = line.size();
        out.push_back({pos, line[0] == '@' ? record : is_level ? level : directive});
        out.push_back({pos + end, 0});
      }
    }
    if (nl == std::string_view::npos) break;
    out.push_back({nl, 0});
    pos = nl + 1;
  }
  return out;
}

const TextSelect::Layout& TextSelect::layout(ImGuiID id, std::string_view text, ImFont* font, float size, float width) {
  auto& l = cache_[id];
  std::size_t h = std::hash<std::string_view>{}(text);
  if (l.hash != h || l.font != font || std::fabs(l.size - size) > 0.01f || std::fabs(l.width - width) > 0.5f || l.lines.empty()) {
    l.hash = h;
    l.font = font;
    l.size = size;
    l.width = width;
    l.lines = wrap_lines(text, font, size, width);
    l.max_w = 0;
    for (const auto& [b, e] : l.lines) l.max_w = std::max(l.max_w, width_of(font, size, text, b, e));
  }
  l.used = frame_;
  return l;
}

void TextSelect::begin() {
  ++frame_;
  inside_ = false;
  ends_seen_ = 0;
  parts_n_ = 0;
  parts_.clear();
  pressed_on_text_ = false;
  have_target_ = false;
  first_piece_ = last_piece_ = 0;
  last_size_ = 0;
  if (frame_ % 600 == 0) {
    std::erase_if(cache_, [&](const auto& kv) { return kv.second.used < frame_ - 600; });
  }
}

void TextSelect::clear() {
  anchor_ = focus_ = Pos{};
  dragging_ = false;
  selected_.clear();
}

void TextSelect::text(const char* id, std::string_view s, ImVec4 color, ImFont* font, const std::vector<TextSpan>* spans) {
  if (font != nullptr) ImGui::PushFont(font);
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  ImGuiID pid = ImGui::GetID(id);
  ImFont* f = ImGui::GetFont();
  float fs = ImGui::GetFontSize();
  float scale = fs / f->FontSize;
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float wrap_pos = win->DC.TextWrapPos;
  float width = wrap_pos >= 0.0f ? ImGui::CalcWrapWidthForPos(pos, wrap_pos) : ImGui::GetContentRegionAvail().x;
  width = std::max(width, fs * 2);
  const Layout& l = layout(pid, s, f, fs, width);
  float lh = fs;
  auto n_lines = static_cast<int>(std::max<std::size_t>(1, l.lines.size()));
  float h = lh * static_cast<float>(n_lines);
  // An item of its own: a press holds the mouse until it is released (the drag selects, the
  // window does not move), hovering respects windows and popups over it.
  ImVec2 size(std::max(1.0f, l.max_w), h);
  ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
  ImGui::ItemSize(size);
  bool visible = ImGui::ItemAdd(bb, pid);
  bool hovered = false;
  bool held = false;
  bool pressed = visible && ImGui::ButtonBehavior(bb, pid, &hovered, &held, static_cast<int>(ImGuiButtonFlags_PressedOnClick) | static_cast<int>(ImGuiButtonFlags_MouseButtonLeft));
  if (first_piece_ == 0) first_piece_ = pid;
  last_piece_ = pid;
  last_size_ = s.size();

  // The part of this piece inside the selection (pieces come in order: between the two ends
  // everything is selected).
  std::size_t sb = 0;
  std::size_t se = 0;
  if (anchor_.piece != 0) {
    bool a = anchor_.piece == pid;
    bool z = focus_.piece == pid;
    if (a && z) {
      sb = std::min(anchor_.off, focus_.off);
      se = std::max(anchor_.off, focus_.off);
      ends_seen_ += 2;
    } else if (a || z) {
      std::size_t off = a ? anchor_.off : focus_.off;
      if (!inside_) {
        sb = off;
        se = s.size();
      } else {
        sb = 0;
        se = off;
      }
      inside_ = !inside_;
      ++ends_seen_;
    } else if (inside_) {
      sb = 0;
      se = s.size();
    }
    sb = char_start(s, sb);
    se = char_start(s, std::min(se, s.size()));
    if (se > sb) {
      if (parts_n_++ > 0) parts_ += '\n';
      parts_.append(s.substr(sb, se - sb));
    }
  }

  ImGuiIO& io = ImGui::GetIO();
  auto off_at = [&](ImVec2 m) -> std::size_t {
    if (l.lines.empty()) return 0;
    int li = std::clamp(static_cast<int>((m.y - pos.y) / lh), 0, static_cast<int>(l.lines.size()) - 1);
    const auto& [lb, le] = l.lines[static_cast<std::size_t>(li)];
    return offset_at_x(f, scale, s, lb, le, m.x - pos.x);
  };
  if (hovered || held) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
  if (pressed) {
    pressed_on_text_ = true;
    std::size_t off = off_at(io.MousePos);
    int clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
    if (clicks >= 3) {
      anchor_ = {pid, 0};
      focus_ = {pid, s.size()};
      dragging_ = false;
    } else if (clicks == 2) {
      auto [wb, we] = word_at(s, off);
      anchor_ = {pid, wb};
      focus_ = {pid, we};
      dragging_ = false;
    } else if (io.KeyShift && anchor_.piece != 0) {
      focus_ = {pid, off};
      dragging_ = true;
    } else {
      anchor_ = focus_ = {pid, off};
      dragging_ = true;
    }
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    menu_piece_.assign(s);
    open_menu_ = true;
  }
  // While dragging: the place under the mouse — in the last piece that starts above it.
  if (dragging_) {
    float my = io.MousePos.y;
    if (my >= pos.y) {
      have_target_ = true;
      target_ = my < pos.y + h ? Pos{pid, off_at(io.MousePos)} : Pos{pid, s.size()};
    } else if (!have_target_) {
      have_target_ = true;
      target_ = Pos{pid, 0};
    }
  }

  if (visible && !l.lines.empty()) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 sel_col = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
    ImU32 col = ImGui::GetColorU32(color);
    int first = std::max(0, static_cast<int>((dl->GetClipRectMin().y - pos.y) / lh));
    int last = std::min(static_cast<int>(l.lines.size()) - 1, static_cast<int>((dl->GetClipRectMax().y - pos.y) / lh));
    for (int i = first; i <= last; ++i) {
      const auto& [lb, le] = l.lines[static_cast<std::size_t>(i)];
      float y = pos.y + lh * static_cast<float>(i);
      if (se > sb && se > lb && sb <= le) {
        std::size_t a = std::max(sb, lb);
        std::size_t b = std::min(se, le);
        float x0 = pos.x + width_of(f, fs, s, lb, a);
        float x1 = pos.x + width_of(f, fs, s, lb, b);
        if (se > le) x1 += fs * 0.3f;  // the line break is selected too
        if (x1 > x0) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + lh), sel_col);
      }
      if (spans == nullptr || spans->empty()) {
        dl->AddText(f, fs, ImVec2(pos.x, y), col, s.data() + lb, s.data() + le);
        continue;
      }
      auto it = std::upper_bound(spans->begin(), spans->end(), lb, [](std::size_t v, const TextSpan& t) { return v < t.at; });
      ImU32 c = it == spans->begin() || std::prev(it)->color == 0 ? col : std::prev(it)->color;
      std::size_t p = lb;
      float x = pos.x;
      while (p < le) {
        std::size_t next = it == spans->end() ? le : std::min(le, std::max(p, it->at));
        if (next > p) {
          dl->AddText(f, fs, ImVec2(x, y), c, s.data() + p, s.data() + next);
          x += width_of(f, fs, s, p, next);
          p = next;
        }
        if (it != spans->end() && it->at <= p) {
          c = it->color == 0 ? col : it->color;
          ++it;
        }
      }
    }
  }
  if (font != nullptr) ImGui::PopFont();
}

void TextSelect::end() {
  ImGuiIO& io = ImGui::GetIO();
  if (dragging_) {
    if (have_target_) focus_ = target_;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging_ = false;
    // Dragging past the edge of the region scrolls it.
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    float top = w->InnerRect.Min.y;
    float bottom = w->InnerRect.Max.y;
    if (io.MousePos.y < top) ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - (top - io.MousePos.y) * 0.3f));
    if (io.MousePos.y > bottom) ImGui::SetScrollY(std::min(ImGui::GetScrollMaxY(), ImGui::GetScrollY() + (io.MousePos.y - bottom) * 0.3f));
  }
  bool region_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  if (!pressed_on_text_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && region_hovered && !ImGui::IsAnyItemHovered()) clear();
  // An end that was not drawn this frame (its text is gone): no selection.
  if (!pressed_on_text_ && !dragging_ && anchor_.piece != 0 && ends_seen_ < 2) clear();
  if (anchor_.piece != 0) selected_ = parts_;
  bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  if (focused && !io.WantTextInput) {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) && !selected_.empty()) ImGui::SetClipboardText(selected_.c_str());
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A) && first_piece_ != 0) {
      anchor_ = {first_piece_, 0};
      focus_ = {last_piece_, last_size_};
    }
  }
  if (open_menu_) {
    ImGui::OpenPopup("##text_menu");
    open_menu_ = false;
  }
  if (ImGui::BeginPopup("##text_menu")) {
    if (ImGui::MenuItem(with_icon(Icon::Copy, tr("Copy")).c_str(), "Ctrl+C", false, !selected_.empty())) {
      ImGui::SetClipboardText(selected_.c_str());
    }
    if (ImGui::MenuItem(tr("Copy the whole message"))) ImGui::SetClipboardText(menu_piece_.c_str());
    if (ImGui::MenuItem(tr("Select all"), "Ctrl+A") && first_piece_ != 0) {
      anchor_ = {first_piece_, 0};
      focus_ = {last_piece_, last_size_};
    }
    ImGui::EndPopup();
  }
}

}  // namespace kk::gui::ui
