#include "icons.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace kk::gui {

namespace {

struct V {
  float x, y;
};
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y}; }
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y}; }
V operator*(V a, float s) { return {a.x * s, a.y * s}; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y; }
float len(V a) { return std::sqrt(dot(a, a)); }

// --- signed distance primitives (em units, y down; negative inside) ---------------------
float circle(V p, V c, float r) { return len(p - c) - r; }
float ring(V p, V c, float r, float t) { return std::fabs(len(p - c) - r) - t; }
float box(V p, V a, V b, float r = 0) {
  V c = (a + b) * 0.5f;
  V h = (b - a) * 0.5f;
  V q{std::fabs(p.x - c.x) - h.x + r, std::fabs(p.y - c.y) - h.y + r};
  V m{std::max(q.x, 0.0f), std::max(q.y, 0.0f)};
  return len(m) + std::min(std::max(q.x, q.y), 0.0f) - r;
}
float seg(V p, V a, V b, float t) {
  V pa = p - a, ba = b - a;
  float h = std::clamp(dot(pa, ba) / dot(ba, ba), 0.0f, 1.0f);
  return len(pa - ba * h) - t;
}
float poly(V p, const std::vector<V>& v) {
  float d = dot(p - v[0], p - v[0]);
  float s = 1.0f;
  for (std::size_t i = 0, j = v.size() - 1; i < v.size(); j = i, ++i) {
    V e = v[j] - v[i], w = p - v[i];
    V b = w - e * std::clamp(dot(w, e) / dot(e, e), 0.0f, 1.0f);
    d = std::min(d, dot(b, b));
    bool c1 = p.y >= v[i].y, c2 = p.y < v[j].y, c3 = e.x * w.y > e.y * w.x;
    if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
  }
  return s * std::sqrt(d);
}
float U(float a, float b) { return std::min(a, b); }
float S(float a, float b) { return std::max(a, -b); }  // a minus b
float I(float a, float b) { return std::max(a, b); }

using Sdf = std::function<float(V)>;

constexpr float T = 0.055f;  // stroke half-width

const std::array<Sdf, static_cast<std::size_t>(Icon::Count)>& shapes() {
  static const std::array<Sdf, static_cast<std::size_t>(Icon::Count)> s = {
      // Folder
      [](V p) { return U(box(p, {0.08f, 0.30f}, {0.92f, 0.84f}, 0.07f), box(p, {0.08f, 0.18f}, {0.46f, 0.36f}, 0.06f)); },
      // FolderOpen
      [](V p) {
        float back = U(box(p, {0.08f, 0.18f}, {0.44f, 0.34f}, 0.05f), box(p, {0.08f, 0.26f}, {0.80f, 0.80f}, 0.06f));
        float front = poly(p, {{0.22f, 0.44f}, {0.98f, 0.44f}, {0.84f, 0.84f}, {0.06f, 0.84f}});
        return U(S(back, box(p, {0.14f, 0.38f}, {0.86f, 0.90f}, 0.0f)), front);
      },
      // File
      [](V p) {
        float page = S(box(p, {0.20f, 0.08f}, {0.80f, 0.92f}, 0.07f), poly(p, {{0.58f, 0.0f}, {0.9f, 0.0f}, {0.9f, 0.32f}}));
        float lines = U(U(seg(p, {0.32f, 0.48f}, {0.68f, 0.48f}, 0.035f), seg(p, {0.32f, 0.62f}, {0.68f, 0.62f}, 0.035f)),
                        seg(p, {0.32f, 0.76f}, {0.56f, 0.76f}, 0.035f));
        return S(page, lines);
      },
      // Cube (a leaf block)
      [](V p) {
        float hex = poly(p, {{0.5f, 0.06f}, {0.90f, 0.28f}, {0.90f, 0.72f}, {0.5f, 0.94f}, {0.10f, 0.72f}, {0.10f, 0.28f}});
        float y = U(U(seg(p, {0.5f, 0.5f}, {0.5f, 0.94f}, 0.04f), seg(p, {0.5f, 0.5f}, {0.10f, 0.28f}, 0.04f)),
                    seg(p, {0.5f, 0.5f}, {0.90f, 0.28f}, 0.04f));
        return S(hex, y);
      },
      // Play (entry)
      [](V p) { return poly(p, {{0.26f, 0.14f}, {0.86f, 0.5f}, {0.26f, 0.86f}}) - 0.02f; },
      // Library (shared)
      [](V p) {
        return U(U(box(p, {0.10f, 0.16f}, {0.30f, 0.88f}, 0.04f), box(p, {0.36f, 0.24f}, {0.56f, 0.88f}, 0.04f)),
                 poly(p, {{0.62f, 0.30f}, {0.78f, 0.24f}, {0.94f, 0.84f}, {0.78f, 0.90f}}));
      },
      // Gear
      [](V p) {
        float d = circle(p, {0.5f, 0.5f}, 0.30f);
        for (int i = 0; i < 8; ++i) {
          float a = static_cast<float>(i) * 3.14159265f / 4.0f;
          V dir{std::cos(a), std::sin(a)};
          d = U(d, seg(p, V{0.5f, 0.5f} + dir * 0.28f, V{0.5f, 0.5f} + dir * 0.40f, 0.075f));
        }
        return S(d, circle(p, {0.5f, 0.5f}, 0.12f));
      },
      // Search
      [](V p) { return U(ring(p, {0.42f, 0.42f}, 0.25f, T + 0.01f), seg(p, {0.62f, 0.62f}, {0.86f, 0.86f}, 0.085f)); },
      // Layers (all levels)
      [](V p) {
        float top = poly(p, {{0.5f, 0.10f}, {0.92f, 0.32f}, {0.5f, 0.54f}, {0.08f, 0.32f}});
        float mid = U(seg(p, {0.10f, 0.52f}, {0.5f, 0.72f}, T), seg(p, {0.5f, 0.72f}, {0.90f, 0.52f}, T));
        float low = U(seg(p, {0.10f, 0.70f}, {0.5f, 0.90f}, T), seg(p, {0.5f, 0.90f}, {0.90f, 0.70f}, T));
        return U(top, U(mid, low));
      },
      // Link
      [](V p) {
        auto chain = [&](V a, V b) { return std::fabs(seg(p, a, b, 0.0f) - 0.14f) - T; };
        return U(chain({0.24f, 0.62f}, {0.42f, 0.44f}), chain({0.58f, 0.56f}, {0.76f, 0.38f}));
      },
      // Undo: an arc over the top with the arrow head at its left end
      [](V p) {
        V c{0.56f, 0.56f};
        float r = 0.28f;
        float arc = I(ring(p, c, r, T), U(p.y - c.y, c.x - p.x));
        float head = poly(p, {{c.x - r - 0.16f, c.y - 0.04f}, {c.x - r + 0.16f, c.y - 0.04f}, {c.x - r, c.y + 0.20f}});
        return U(arc, head);
      },
      // Plus
      [](V p) { return U(seg(p, {0.5f, 0.16f}, {0.5f, 0.84f}, 0.07f), seg(p, {0.16f, 0.5f}, {0.84f, 0.5f}, 0.07f)); },
      // Close
      [](V p) { return U(seg(p, {0.22f, 0.22f}, {0.78f, 0.78f}, 0.065f), seg(p, {0.78f, 0.22f}, {0.22f, 0.78f}, 0.065f)); },
      // Check
      [](V p) { return U(seg(p, {0.16f, 0.52f}, {0.40f, 0.76f}, 0.07f), seg(p, {0.40f, 0.76f}, {0.86f, 0.26f}, 0.07f)); },
      // Warning
      [](V p) {
        float tri = poly(p, {{0.5f, 0.08f}, {0.95f, 0.88f}, {0.05f, 0.88f}}) - 0.02f;
        return S(tri, U(seg(p, {0.5f, 0.36f}, {0.5f, 0.60f}, 0.05f), circle(p, {0.5f, 0.74f}, 0.055f)));
      },
      // Error
      [](V p) {
        return S(circle(p, {0.5f, 0.5f}, 0.42f),
                 U(seg(p, {0.34f, 0.34f}, {0.66f, 0.66f}, 0.055f), seg(p, {0.66f, 0.34f}, {0.34f, 0.66f}, 0.055f)));
      },
      // Info
      [](V p) {
        return S(circle(p, {0.5f, 0.5f}, 0.42f), U(seg(p, {0.5f, 0.46f}, {0.5f, 0.72f}, 0.055f), circle(p, {0.5f, 0.31f}, 0.06f)));
      },
      // Ticket
      [](V p) {
        float t = box(p, {0.06f, 0.24f}, {0.94f, 0.76f}, 0.06f);
        t = S(t, U(circle(p, {0.06f, 0.5f}, 0.11f), circle(p, {0.94f, 0.5f}, 0.11f)));
        return S(t, seg(p, {0.64f, 0.30f}, {0.64f, 0.70f}, 0.025f));
      },
      // Agent
      [](V p) {
        float head = box(p, {0.16f, 0.30f}, {0.84f, 0.86f}, 0.12f);
        head = S(head, U(circle(p, {0.36f, 0.54f}, 0.075f), circle(p, {0.64f, 0.54f}, 0.075f)));
        head = S(head, seg(p, {0.38f, 0.72f}, {0.62f, 0.72f}, 0.03f));
        return U(head, U(seg(p, {0.5f, 0.12f}, {0.5f, 0.30f}, 0.04f), circle(p, {0.5f, 0.12f}, 0.07f)));
      },
      // Hammer
      [](V p) {
        return U(seg(p, {0.22f, 0.84f}, {0.60f, 0.46f}, 0.07f),
                 poly(p, {{0.44f, 0.22f}, {0.64f, 0.08f}, {0.92f, 0.36f}, {0.78f, 0.56f}}));
      },
      // Coins
      [](V p) {
        return U(S(circle(p, {0.38f, 0.42f}, 0.30f), circle(p, {0.38f, 0.42f}, 0.17f)), S(circle(p, {0.64f, 0.64f}, 0.28f), circle(p, {0.64f, 0.64f}, 0.15f)));
      },
      // Changes (compare)
      [](V p) {
        float a = U(ring(p, {0.28f, 0.24f}, 0.12f, T), ring(p, {0.72f, 0.76f}, 0.12f, T));
        float l1 = U(seg(p, {0.28f, 0.36f}, {0.28f, 0.70f}, T), seg(p, {0.28f, 0.70f}, {0.58f, 0.70f}, T));
        float l2 = U(seg(p, {0.72f, 0.64f}, {0.72f, 0.30f}, T), seg(p, {0.72f, 0.30f}, {0.42f, 0.30f}, T));
        return U(a, U(l1, l2));
      },
      // Eye
      [](V p) {
        float almond = I(circle(p, {0.5f, 0.92f}, 0.62f), circle(p, {0.5f, 0.08f}, 0.62f));
        return U(S(almond, circle(p, {0.5f, 0.5f}, 0.22f)), circle(p, {0.5f, 0.5f}, 0.12f));
      },
      // Fit (corners)
      [](V p) {
        float d = 1e9f;
        for (V c : {V{0.14f, 0.14f}, V{0.86f, 0.14f}, V{0.14f, 0.86f}, V{0.86f, 0.86f}}) {
          float sx = c.x < 0.5f ? 1.0f : -1.0f, sy = c.y < 0.5f ? 1.0f : -1.0f;
          d = U(d, U(seg(p, c, c + V{0.26f * sx, 0}, T), seg(p, c, c + V{0, 0.26f * sy}, T)));
        }
        return d;
      },
      // Home
      [](V p) { return U(poly(p, {{0.5f, 0.10f}, {0.92f, 0.50f}, {0.08f, 0.50f}}), S(box(p, {0.20f, 0.46f}, {0.80f, 0.90f}, 0.03f), box(p, {0.42f, 0.62f}, {0.58f, 0.92f}))); },
      // ArrowUp
      [](V p) { return U(poly(p, {{0.5f, 0.10f}, {0.86f, 0.48f}, {0.14f, 0.48f}}), seg(p, {0.5f, 0.40f}, {0.5f, 0.88f}, 0.08f)); },
      // Schema (connected nodes)
      [](V p) {
        float n = U(U(box(p, {0.06f, 0.10f}, {0.40f, 0.38f}, 0.05f), box(p, {0.60f, 0.36f}, {0.94f, 0.64f}, 0.05f)),
                    box(p, {0.06f, 0.62f}, {0.40f, 0.90f}, 0.05f));
        return U(n, U(seg(p, {0.40f, 0.24f}, {0.60f, 0.50f}, 0.035f), seg(p, {0.40f, 0.76f}, {0.60f, 0.50f}, 0.035f)));
      },
      // Code
      [](V p) {
        return U(U(U(seg(p, {0.30f, 0.26f}, {0.08f, 0.5f}, T), seg(p, {0.08f, 0.5f}, {0.30f, 0.74f}, T)),
                   U(seg(p, {0.70f, 0.26f}, {0.92f, 0.5f}, T), seg(p, {0.92f, 0.5f}, {0.70f, 0.74f}, T))),
                 seg(p, {0.58f, 0.16f}, {0.42f, 0.84f}, T));
      },
      // Sliders (inspector)
      [](V p) {
        float l = U(U(seg(p, {0.10f, 0.24f}, {0.90f, 0.24f}, 0.035f), seg(p, {0.10f, 0.5f}, {0.90f, 0.5f}, 0.035f)),
                    seg(p, {0.10f, 0.76f}, {0.90f, 0.76f}, 0.035f));
        return U(l, U(U(circle(p, {0.66f, 0.24f}, 0.10f), circle(p, {0.34f, 0.5f}, 0.10f)), circle(p, {0.58f, 0.76f}, 0.10f)));
      },
      // Files (explorer)
      [](V p) {
        float back = box(p, {0.30f, 0.06f}, {0.86f, 0.72f}, 0.06f);
        float front = box(p, {0.14f, 0.26f}, {0.70f, 0.94f}, 0.06f);
        return U(S(back, box(p, {0.08f, 0.20f}, {0.76f, 1.0f}, 0.08f)), front);
      },
      // Wave (drift)
      [](V p) {
        float d = 1e9f;
        for (float y0 : {0.34f, 0.66f}) {
          V a{0.08f, y0}, b{0.30f, y0 - 0.12f}, c{0.52f, y0}, e{0.74f, y0 - 0.12f}, f{0.92f, y0 - 0.02f};
          d = U(d, U(U(seg(p, a, b, T), seg(p, b, c, T)), U(seg(p, c, e, T), seg(p, e, f, T))));
        }
        return d;
      },
      // Dot
      [](V p) { return circle(p, {0.5f, 0.5f}, 0.22f); },
      // Pin (highlight kept)
      [](V p) {
        float head = U(circle(p, {0.5f, 0.32f}, 0.22f), poly(p, {{0.28f, 0.40f}, {0.72f, 0.40f}, {0.5f, 0.70f}}));
        return U(S(head, circle(p, {0.5f, 0.32f}, 0.08f)), seg(p, {0.5f, 0.62f}, {0.5f, 0.92f}, 0.035f));
      },
      // Card (KSL card)
      [](V p) {
        float c = box(p, {0.08f, 0.16f}, {0.92f, 0.84f}, 0.08f);
        float lines = U(U(seg(p, {0.22f, 0.36f}, {0.40f, 0.36f}, 0.035f), seg(p, {0.50f, 0.36f}, {0.78f, 0.36f}, 0.035f)),
                        U(seg(p, {0.22f, 0.52f}, {0.40f, 0.52f}, 0.035f), seg(p, {0.50f, 0.52f}, {0.70f, 0.52f}, 0.035f)));
        return S(c, U(lines, U(seg(p, {0.22f, 0.68f}, {0.40f, 0.68f}, 0.035f), seg(p, {0.50f, 0.68f}, {0.74f, 0.68f}, 0.035f))));
      },
      // Stop (a square)
      [](V p) { return box(p, {0.22f, 0.22f}, {0.78f, 0.78f}, 0.06f); },
      // Trash
      [](V p) {
        float can = S(box(p, {0.22f, 0.30f}, {0.78f, 0.92f}, 0.06f), U(seg(p, {0.40f, 0.44f}, {0.40f, 0.80f}, 0.035f),
                                                                       seg(p, {0.60f, 0.44f}, {0.60f, 0.80f}, 0.035f)));
        float lid = U(seg(p, {0.12f, 0.20f}, {0.88f, 0.20f}, 0.05f), box(p, {0.38f, 0.06f}, {0.62f, 0.18f}, 0.03f));
        return U(can, lid);
      },
      // Send (paper plane)
      [](V p) {
        float plane = poly(p, {{0.06f, 0.46f}, {0.94f, 0.10f}, {0.62f, 0.92f}, {0.46f, 0.58f}});
        return S(plane, seg(p, {0.46f, 0.58f}, {0.94f, 0.10f}, 0.03f));
      },
      // Chat (speech bubble with lines)
      [](V p) {
        float bubble = U(box(p, {0.06f, 0.12f}, {0.94f, 0.70f}, 0.12f), poly(p, {{0.22f, 0.62f}, {0.44f, 0.62f}, {0.20f, 0.90f}}));
        return S(bubble, U(seg(p, {0.24f, 0.32f}, {0.76f, 0.32f}, 0.04f), seg(p, {0.24f, 0.50f}, {0.60f, 0.50f}, 0.04f)));
      },
      // Copy (two sheets)
      [](V p) {
        float back = S(box(p, {0.10f, 0.08f}, {0.62f, 0.66f}, 0.07f), box(p, {0.10f + 2 * T, 0.08f + 2 * T}, {0.62f - 2 * T, 0.66f - 2 * T}, 0.03f));
        float front = box(p, {0.36f, 0.32f}, {0.90f, 0.92f}, 0.07f);
        float hole = box(p, {0.36f + 2 * T, 0.32f + 2 * T}, {0.90f - 2 * T, 0.92f - 2 * T}, 0.03f);
        return U(S(back, box(p, {0.30f, 0.26f}, {0.96f, 0.98f}, 0.08f)), S(front, hole));
      },
      // Windows (a window with panes)
      [](V p) {
        float frame = S(box(p, {0.08f, 0.12f}, {0.92f, 0.88f}, 0.08f), box(p, {0.08f + 2 * T, 0.30f}, {0.92f - 2 * T, 0.88f - 2 * T}, 0.02f));
        return U(frame, seg(p, {0.42f, 0.30f}, {0.42f, 0.84f}, T));
      },
      // Pencil
      [](V p) {
        float body = poly(p, {{0.70f, 0.10f}, {0.90f, 0.30f}, {0.36f, 0.84f}, {0.16f, 0.64f}});
        float tip = poly(p, {{0.16f, 0.64f}, {0.36f, 0.84f}, {0.08f, 0.92f}});
        return U(body, tip);
      },
      // Bolt (Auto)
      [](V p) { return poly(p, {{0.58f, 0.04f}, {0.20f, 0.56f}, {0.46f, 0.56f}, {0.38f, 0.96f}, {0.80f, 0.40f}, {0.54f, 0.40f}}); },
      // ChevronRight
      [](V p) { return U(seg(p, {0.36f, 0.20f}, {0.66f, 0.50f}, 0.07f), seg(p, {0.66f, 0.50f}, {0.36f, 0.80f}, 0.07f)); },
      // ChevronDown
      [](V p) { return U(seg(p, {0.20f, 0.36f}, {0.50f, 0.66f}, 0.07f), seg(p, {0.50f, 0.66f}, {0.80f, 0.36f}, 0.07f)); },
      // Ghost (a dashed block: preview of a change)
      [](V p) {
        float outline = S(box(p, {0.08f, 0.18f}, {0.92f, 0.82f}, 0.10f), box(p, {0.08f + 2 * T, 0.18f + 2 * T}, {0.92f - 2 * T, 0.82f - 2 * T}, 0.06f));
        float gaps = U(U(box(p, {0.30f, 0.0f}, {0.40f, 1.0f}), box(p, {0.60f, 0.0f}, {0.70f, 1.0f})), box(p, {0.0f, 0.45f}, {1.0f, 0.55f}));
        return S(outline, gaps);
      },
  };
  return s;
}

struct GlyphSet {
  int size = 0;
  std::array<int, static_cast<std::size_t>(Icon::Count)> rects{};
};
std::vector<GlyphSet> g_sets;

}  // namespace

const char* icon(Icon i) {
  static std::array<std::string, static_cast<std::size_t>(Icon::Count)> text = [] {
    std::array<std::string, static_cast<std::size_t>(Icon::Count)> t;
    for (std::size_t k = 0; k < t.size(); ++k) {
      unsigned cp = 0xE000u + static_cast<unsigned>(k);
      t[k] = {static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), static_cast<char>(0x80 | (cp & 0x3F))};
    }
    return t;
  }();
  return text[static_cast<std::size_t>(i)].c_str();
}

std::string with_icon(Icon i, const std::string& text) { return std::string(icon(i)) + "  " + text; }

void add_icon_glyphs(ImFont* font, float size_px) {
  GlyphSet set;
  set.size = std::max(8, static_cast<int>(std::lround(size_px * 0.92f)));
  float off_y = std::round((size_px - static_cast<float>(set.size)) * 0.5f + size_px * 0.06f);
  for (std::size_t k = 0; k < set.rects.size(); ++k) {
    set.rects[k] = ImGui::GetIO().Fonts->AddCustomRectFontGlyph(font, static_cast<ImWchar>(0xE000 + k), set.size, set.size,
                                                               static_cast<float>(set.size) + 1.0f, ImVec2(0.0f, off_y));
  }
  g_sets.push_back(set);
}

void rasterize_icon_glyphs() {
  auto* atlas = ImGui::GetIO().Fonts;
  unsigned char* pixels = nullptr;
  int w = 0;
  int h = 0;
  atlas->GetTexDataAsRGBA32(&pixels, &w, &h);
  if (pixels == nullptr) return;
  for (const auto& set : g_sets) {
  const float n = static_cast<float>(set.size);
  for (std::size_t k = 0; k < set.rects.size(); ++k) {
    const auto* r = atlas->GetCustomRectByIndex(set.rects[k]);
    const auto& sdf = shapes()[k];
    for (int y = 0; y < r->Height; ++y) {
      for (int x = 0; x < r->Width; ++x) {
        // Distance at the pixel center, in pixels; 1-pixel wide anti-aliased edge.
        V p{(static_cast<float>(x) + 0.5f) / n, (static_cast<float>(y) + 0.5f) / n};
        float d = sdf(p) * n;
        float a = std::clamp(0.5f - d, 0.0f, 1.0f);
        unsigned char* px = pixels + (static_cast<std::size_t>(r->Y + y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(r->X + x)) * 4;
        px[0] = px[1] = px[2] = 255;
        px[3] = static_cast<unsigned char>(std::lround(a * 255.0f));
      }
    }
  }
  }
}

}  // namespace kk::gui
