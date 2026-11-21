#include "markdown_view.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <string_view>

#include "icons.hpp"
#include "theme.hpp"

namespace kk::gui {

namespace md = core::md;

namespace {

const ImU32 kDim = IM_COL32(150, 150, 150, 255);
const ImU32 kHeading = IM_COL32(236, 236, 236, 255);
const ImU32 kLinkNote = IM_COL32(120, 170, 240, 255);
const ImU32 kLinkOut = IM_COL32(55, 148, 255, 255);
const ImU32 kLinkNowhere = IM_COL32(150, 150, 175, 255);
const ImU32 kCodeFg = IM_COL32(206, 145, 120, 255);
const ImU32 kCodeBg = IM_COL32(255, 255, 255, 16);
const ImU32 kBlockBg = IM_COL32(24, 24, 24, 255);
const ImU32 kBorder = IM_COL32(62, 62, 62, 255);
const ImU32 kHeaderBg = IM_COL32(40, 40, 42, 255);
const ImU32 kQuoteBar = IM_COL32(85, 85, 85, 255);

int utf8_len(char c) {
  auto u = static_cast<unsigned char>(c);
  return u < 0x80 ? 1 : (u >> 5) == 6 ? 2 : (u >> 4) == 14 ? 3 : (u >> 3) == 30 ? 4 : 1;
}

struct Hit {
  ImVec2 a, b;
  const md::Run* run;
};

struct View {
  ImDrawList* dl = nullptr;
  float scale = 1.0f;  // the current font's size over its size in the atlas
  float clip_top = 0, clip_bottom = 0;
  const void* hot = nullptr;  // the link under the mouse in the last frame
  const MdLinkKind* kind = nullptr;
  ImU32 text = 0;
  std::vector<Hit> hits;
  struct Span {
    float top, bottom;
    int line;
  };
  std::vector<Span> spans;

  [[nodiscard]] float size(const ImFont* f) const { return f->FontSize * scale; }
  [[nodiscard]] float width(ImFont* f, const char* b, const char* e) const { return f->CalcTextSizeA(size(f), FLT_MAX, 0.0f, b, e).x; }
  [[nodiscard]] bool visible(float y0, float y1) const { return y1 >= clip_top && y0 <= clip_bottom; }
};

struct Look {
  ImFont* font = nullptr;
  ImU32 color = 0;
  bool strike = false;
  bool code = false;
  bool link = false;
};

Look look_of(const View& v, const md::Run& r, ImFont* base, ImU32 base_color, bool heading) {
  Look l;
  l.strike = r.strike;
  l.code = r.code;
  l.link = !r.href.empty();
  if (r.code) {
    l.font = theme::mono_font();
  } else if (r.image) {
    l.font = theme::ui_font();
  } else if (heading) {
    l.font = base;
  } else {
    l.font = r.bold || r.italic ? theme::md_font(r.bold, r.italic) : base;
  }
  l.color = r.code ? kCodeFg : base_color;
  if (l.link) {
    int k = r.image ? kMdOut : (v.kind != nullptr && *v.kind) ? (*v.kind)(r.href, r.wiki) : kMdOut;
    l.color = k == kMdNote ? kLinkNote : k == kMdOut ? kLinkOut : kLinkNowhere;
  }
  if (r.strike && !l.link) l.color = kDim;
  return l;
}

// One piece of a run on a line: [b, e) at x, `deco` wide up to the end of its word, `full` with the
// spaces after it.
void put(View& v, const Look& lk, const md::Run& r, const char* b, const char* e, float x, float y, float lh, float deco, float full) {
  float fs = v.size(lk.font);
  float ty = y + (lh - fs) * 0.5f;
  if (lk.link) v.hits.push_back({ImVec2(x, y), ImVec2(x + full, y + lh), &r});
  if (!v.visible(y, y + lh)) return;
  if (lk.code) v.dl->AddRectFilled(ImVec2(x - 2, ty - 1), ImVec2(x + deco + 2, ty + fs + 1), kCodeBg, 3.0f);
  v.dl->AddText(lk.font, fs, ImVec2(x, ty), lk.color, b, e);
  if (lk.strike) v.dl->AddLine(ImVec2(x, ty + fs * 0.55f), ImVec2(x + deco, ty + fs * 0.55f), lk.color);
  if (lk.link && v.hot == &r) v.dl->AddLine(ImVec2(x, ty + fs + 1), ImVec2(x + full, ty + fs + 1), lk.color);
}

struct Flow {
  float bottom = 0;  // below the last line
  float right = 0;   // the right end of the longest line
  int lines = 1;
};

// Lays the runs out from x0 within `w`, the first line at y (drawn when `draw`).
Flow flow(View& v, const std::vector<md::Run>& runs, ImFont* base, ImU32 color, float x0, float w, float y, bool heading,
          bool draw) {
  float lh = v.size(base) * 1.4f;
  Flow out;
  out.right = x0;
  float x = x0;
  bool line_empty = true;
  auto new_line = [&] {
    x = x0;
    y += lh;
    line_empty = true;
    ++out.lines;
  };
  for (const auto& r : runs) {
    if (r.text == "\n") {
      new_line();
      continue;
    }
    Look lk = look_of(v, r, base, color, heading);
    std::string shown = r.image ? std::string(icon(Icon::File)) + " " + (r.text.empty() ? r.href : r.text) : r.text;
    const char* s = shown.data();
    const char* end = s + shown.size();
    while (s < end) {
      const char* we = s;  // the word, then the spaces after it
      while (we < end && *we != ' ' && *we != '\t') ++we;
      const char* se = we;
      while (se < end && (*se == ' ' || *se == '\t')) ++se;
      if (line_empty && s == we) {  // spaces at the start of a line go
        s = se;
        continue;
      }
      float ww = v.width(lk.font, s, we);
      if (!line_empty && x + ww > x0 + w) {
        new_line();
        continue;
      }
      if (line_empty && ww > w) {  // a word wider than the line: as much of it as fits
        const char* cut = s;
        float cw = 0;
        while (cut < we) {
          const char* next = std::min(we, cut + utf8_len(*cut));
          float tw = v.width(lk.font, s, next);
          if (tw > w && cut > s) break;
          cut = next;
          cw = tw;
        }
        if (draw) put(v, lk, r, s, cut, x, y, lh, cw, cw);
        out.right = std::max(out.right, x + cw);
        s = cut;
        new_line();
        continue;
      }
      float pw = v.width(lk.font, s, se);
      if (draw) put(v, lk, r, s, se, x, y, lh, ww, pw);
      out.right = std::max(out.right, x + ww);
      x += pw;
      line_empty = false;
      s = se;
    }
  }
  out.bottom = y + lh;
  return out;
}

// Code and front matter: monospaced lines on a box, long lines wrapped at the box's edge.
float code_box(View& v, const md::Block& b, float x0, float w, float y, ImU32 color) {
  ImFont* f = theme::mono_font();
  float fs = v.size(f);
  float lh = fs * 1.35f;
  float pad = fs * 0.6f;
  float inner = std::max(w - 2 * pad, fs * 4);
  std::vector<std::pair<const char*, const char*>> rows;
  const char* s = b.code.data();
  const char* end = s + b.code.size();
  while (true) {
    const char* le = std::find(s, end, '\n');
    if (s == le) rows.emplace_back(s, s);
    for (const char* p = s; p < le;) {  // the line, cut where it does not fit
      const char* q = p;
      while (q < le) {
        const char* next = std::min(le, q + utf8_len(*q));
        if (v.width(f, p, next) > inner && q > p) break;
        q = next;
      }
      rows.emplace_back(p, q);
      p = q;
    }
    if (le == end) break;
    s = le + 1;
  }
  float h = static_cast<float>(rows.size()) * lh + 2 * pad;
  if (v.visible(y, y + h)) {
    v.dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + w, y + h), kBlockBg, 4.0f);
    v.dl->AddRect(ImVec2(x0, y), ImVec2(x0 + w, y + h), kBorder, 4.0f);
    if (!b.lang.empty()) {
      float lw = v.width(f, b.lang.data(), b.lang.data() + b.lang.size());
      v.dl->AddText(f, fs * 0.85f, ImVec2(x0 + w - lw - pad, y + pad * 0.4f), kDim, b.lang.c_str());
    }
  }
  float ty = y + pad;
  for (const auto& [rb, re] : rows) {
    if (v.visible(ty, ty + lh)) v.dl->AddText(f, fs, ImVec2(x0 + pad, ty + (lh - fs) * 0.5f), color, rb, re);
    ty += lh;
  }
  return y + h;
}

float table(View& v, const md::Block& b, float x0, float w, float y) {
  std::size_t cols = b.align.size();
  if (cols == 0 || b.rows.empty()) return y;
  ImFont* body = theme::ui_font();
  ImFont* head = theme::md_font(true, false);
  float em = v.size(body);
  float pad = em * 0.5f;
  // Natural widths (one line each), then shrunk to the width there is.
  std::vector<float> nat(cols, 0.0f);
  for (std::size_t r = 0; r < b.rows.size(); ++r) {
    for (std::size_t c = 0; c < cols && c < b.rows[r].size(); ++c) {
      auto f = flow(v, b.rows[r][c], r == 0 ? head : body, v.text, 0, 1e9f, 0, false, false);
      nat[c] = std::max(nat[c], f.right);
    }
  }
  float total = 0;
  for (float n : nat) total += n + 2 * pad;
  std::vector<float> cw(cols);
  for (std::size_t c = 0; c < cols; ++c) {
    cw[c] = total <= w ? nat[c] + 2 * pad : std::max(em * 3, (nat[c] + 2 * pad) * w / total);
  }
  float tw = 0;
  for (float c : cw) tw += c;
  float top = y;
  for (std::size_t r = 0; r < b.rows.size(); ++r) {
    ImFont* f = r == 0 ? head : body;
    float row_h = 0;
    for (std::size_t c = 0; c < cols; ++c) {
      auto fl = flow(v, b.rows[r][c], f, v.text, 0, cw[c] - 2 * pad, 0, false, false);
      row_h = std::max(row_h, fl.bottom);
    }
    row_h += pad;
    if (r == 0 && v.visible(y, y + row_h)) v.dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + tw, y + row_h), kHeaderBg);
    float x = x0;
    for (std::size_t c = 0; c < cols; ++c) {
      float inner = cw[c] - 2 * pad;
      float shift = 0;
      if (nat[c] <= inner && b.align[c] != 0 && b.align[c] != 'l') {
        auto one = flow(v, b.rows[r][c], f, v.text, 0, 1e9f, 0, false, false);
        shift = b.align[c] == 'r' ? inner - one.right : (inner - one.right) * 0.5f;
      }
      flow(v, b.rows[r][c], f, r == 0 ? kHeading : v.text, x + pad + std::max(0.0f, shift), inner, y + pad * 0.5f, false, true);
      x += cw[c];
    }
    y += row_h;
    if (r + 1 < b.rows.size() && v.visible(y - 1, y + 1)) v.dl->AddLine(ImVec2(x0, y), ImVec2(x0 + tw, y), kBorder);
  }
  if (v.visible(top, y)) {
    v.dl->AddRect(ImVec2(x0, top), ImVec2(x0 + tw, y), kBorder);
    float x = x0;
    for (std::size_t c = 0; c + 1 < cols; ++c) {
      x += cw[c];
      v.dl->AddLine(ImVec2(x, top), ImVec2(x, y), kBorder);
    }
  }
  return y;
}

// The box of a task item, checked or not.
void task_box(View& v, float x, float y, float s, bool done) {
  ImVec2 a(x, y);
  ImVec2 b(x + s, y + s);
  if (done) {
    v.dl->AddRectFilled(a, b, theme::kAccent, 3.0f);
    ImVec2 pts[3] = {ImVec2(x + s * 0.22f, y + s * 0.52f), ImVec2(x + s * 0.42f, y + s * 0.72f), ImVec2(x + s * 0.78f, y + s * 0.3f)};
    v.dl->AddPolyline(pts, 3, IM_COL32_WHITE, ImDrawFlags_None, std::max(1.5f, s * 0.12f));
  } else {
    v.dl->AddRect(a, b, kDim, 3.0f, ImDrawFlags_None, 1.2f);
  }
}

}  // namespace

std::optional<MdClick> markdown_view(const char* id, const std::vector<md::Block>& doc, const MdLinkKind& kind, std::size_t max_blocks,
                                     float max_width) {
  ImGui::PushID(id);
  ImGuiStorage* store = ImGui::GetStateStorage();
  const ImGuiID hot_key = ImGui::GetID("##hot");
  View v;
  v.dl = ImGui::GetWindowDrawList();
  v.scale = ImGui::GetFontSize() / ImGui::GetFont()->FontSize;
  v.clip_top = ImGui::GetCurrentWindow()->ClipRect.Min.y;
  v.clip_bottom = ImGui::GetCurrentWindow()->ClipRect.Max.y;
  v.hot = store->GetVoidPtr(hot_key);
  v.kind = &kind;
  v.text = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  float avail = ImGui::GetContentRegionAvail().x;
  if (max_width > 0) avail = std::min(avail, max_width);
  const float w = std::max(40.0f, avail);
  ImFont* body = theme::ui_font();
  const float em = v.size(body);
  const float quote_step = em * 0.9f;
  float y = origin.y;
  const md::Block* prev = nullptr;
  float prev_bottom = y;
  std::size_t shown = 0;
  for (const auto& b : doc) {
    if (shown++ >= max_blocks) {
      v.dl->AddText(body, em, ImVec2(origin.x, y + em * 0.3f), kDim, "…");
      y += em * 1.6f;
      break;
    }
    float gap = 0;
    if (prev != nullptr) {
      gap = b.kind == md::Kind::Heading ? (b.level <= 2 ? em * 0.9f : em * 0.7f)
            : (b.kind == md::Kind::Item && prev->kind == md::Kind::Item) ? em * 0.12f
                                                                          : em * 0.55f;
    }
    float top = y + gap;
    float x0 = origin.x + static_cast<float>(b.quote) * quote_step + static_cast<float>(b.depth) * em * 1.4f;
    float bw = std::max(em * 4, origin.x + w - x0);
    ImU32 text = b.quote > 0 ? IM_COL32(180, 180, 180, 255) : v.text;
    float bottom = top;
    switch (b.kind) {
      case md::Kind::Paragraph: bottom = flow(v, b.runs, body, text, x0, bw, top, false, true).bottom; break;
      case md::Kind::Heading: {
        ImFont* hf = theme::md_heading_font(b.level);
        bottom = flow(v, b.runs, hf, kHeading, x0, bw, top, true, true).bottom;
        if (b.level <= 2) {
          bottom += em * 0.2f;
          if (v.visible(bottom - 1, bottom + 1)) v.dl->AddLine(ImVec2(x0, bottom), ImVec2(x0 + bw, bottom), kBorder);
          bottom += em * 0.15f;
        }
        break;
      }
      case md::Kind::Item: {
        float lh = em * 1.4f;
        bool ordered = b.marker != "-";
        float mw = em * 1.3f;
        if (ordered) mw = std::max(mw, v.width(body, b.marker.data(), b.marker.data() + b.marker.size()) + em * 0.45f);
        if (b.task >= 0) mw = em * 1.45f;
        if (v.visible(top, top + lh)) {
          if (b.task >= 0) {
            float s = em * 0.82f;
            task_box(v, x0 + em * 0.05f, top + (lh - s) * 0.5f, s, b.task == 1);
          } else if (ordered) {
            float mwid = v.width(body, b.marker.data(), b.marker.data() + b.marker.size());
            v.dl->AddText(body, em, ImVec2(x0 + mw - em * 0.35f - mwid, top + (lh - em) * 0.5f), kDim, b.marker.c_str());
          } else {
            static const char* const kBullets[3] = {"•", "◦", "▪"};
            v.dl->AddText(body, em, ImVec2(x0 + em * 0.3f, top + (lh - em) * 0.5f), kDim, kBullets[b.depth % 3]);
          }
        }
        bottom = flow(v, b.runs, body, b.task == 1 ? kDim : text, x0 + mw, bw - mw, top, false, true).bottom;
        break;
      }
      case md::Kind::Code: bottom = code_box(v, b, x0, bw, top, IM_COL32(212, 212, 212, 255)); break;
      case md::Kind::Meta: bottom = code_box(v, b, x0, bw, top, kDim); break;
      case md::Kind::Rule:
        if (v.visible(top, top + em)) v.dl->AddLine(ImVec2(x0, top + em * 0.5f), ImVec2(x0 + bw, top + em * 0.5f), kBorder, 1.5f);
        bottom = top + em;
        break;
      case md::Kind::Table: bottom = table(v, b, x0, bw, top); break;
    }
    // Quote bars, joined to the block before when it is quoted as deep.
    for (int q = 0; q < b.quote; ++q) {
      float bar_top = prev != nullptr && prev->quote > q ? prev_bottom : top;
      float bx = origin.x + static_cast<float>(q) * quote_step + 2;
      if (v.visible(bar_top, bottom)) v.dl->AddRectFilled(ImVec2(bx, bar_top), ImVec2(bx + 3, bottom), kQuoteBar, 1.5f);
    }
    v.spans.push_back({top, bottom, b.line});
    prev = &b;
    prev_bottom = bottom;
    y = bottom;
  }
  const float h = std::max(1.0f, y - origin.y);
  ImGui::SetCursorScreenPos(origin);
  ImGui::InvisibleButton("##md", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  const ImVec2 mouse = ImGui::GetMousePos();
  const md::Run* hot = nullptr;
  if (hovered) {
    for (const auto& hit : v.hits) {
      if (mouse.x >= hit.a.x && mouse.x < hit.b.x && mouse.y >= hit.a.y && mouse.y < hit.b.y) hot = hit.run;
    }
  }
  store->SetVoidPtr(hot_key, const_cast<md::Run*>(hot));
  auto line_at = [&](float my) {
    int line = 0;
    for (const auto& s : v.spans) {
      if (my >= s.top) line = s.line;
    }
    return line;
  };
  std::optional<MdClick> out;
  if (hot != nullptr) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", hot->href.c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      out = MdClick{.href = hot->href, .wiki = hot->wiki, .image = hot->image, .dbl = false, .line = line_at(mouse.y)};
    }
  } else if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    out = MdClick{.href = {}, .wiki = false, .image = false, .dbl = true, .line = line_at(mouse.y)};
  }
  ImGui::PopID();
  return out;
}

}  // namespace kk::gui
