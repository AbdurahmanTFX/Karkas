// Fitting the schema into the part of the canvas that no panel covers (ADR 0021). The canvas
// spans the whole work area and the panels lie on it, so "fit to view" of the node editor would
// put blocks under them; here the view is computed so that the blocks fill the free part.
#include <imgui_node_editor_internal.h>

#include <algorithm>

#include "ui.hpp"

namespace kk::gui {

void fit_canvas(ax::NodeEditor::EditorContext* editor, ImVec2 view_min, ImVec2 view_max, ImVec2 free_min, ImVec2 free_max, float max_zoom) {
  auto* ctx = reinterpret_cast<ax::NodeEditor::Detail::EditorContext*>(editor);
  if (ctx == nullptr) return;
  ImRect content = ctx->GetContentBounds();
  float vw = view_max.x - view_min.x;
  float vh = view_max.y - view_min.y;
  // Insets of the free part inside the canvas (screen pixels).
  float l = std::clamp(free_min.x - view_min.x, 0.0f, vw);
  float r = std::clamp(view_max.x - free_max.x, 0.0f, vw);
  float t = std::clamp(free_min.y - view_min.y, 0.0f, vh);
  float b = std::clamp(view_max.y - free_max.y, 0.0f, vh);
  float fw = vw - l - r;
  float fh = vh - t - b;
  if (content.GetWidth() <= 0 || content.GetHeight() <= 0 || fw < 60 || fh < 60 || vw <= 0 || vh <= 0) {
    ax::NodeEditor::NavigateToContent(0.0f);
    return;
  }
  constexpr float kMargin = 0.06f;  // of the content, on each side
  float zoom = std::min(fw / (content.GetWidth() * (1 + 2 * kMargin)), fh / (content.GetHeight() * (1 + 2 * kMargin)));
  zoom = std::clamp(zoom, 0.02f, max_zoom);
  // The canvas rectangle the whole view shows: the content's center lands in the free part's.
  float tw = vw / zoom;
  float th = vh / zoom;
  ImVec2 c = content.GetCenter();
  ImVec2 tc(c.x + (r - l) / (2 * zoom), c.y + (b - t) / (2 * zoom));
  // NavigateTo(..., zoomIn) grows the rectangle by 5% of its larger side on every side
  // (c_NavigationZoomMargin 0.1 / 2): pass the one that grows into exactly the view.
  constexpr float k = 0.05f;
  float w0 = 0;
  float h0 = 0;
  if (tw >= th) {
    w0 = tw / (1 + 2 * k);
    h0 = th - 2 * k * w0;
  } else {
    h0 = th / (1 + 2 * k);
    w0 = tw - 2 * k * h0;
  }
  if (w0 <= 0 || h0 <= 0) {
    ax::NodeEditor::NavigateToContent(0.0f);
    return;
  }
  ctx->NavigateTo(ImRect(ImVec2(tc.x - w0 / 2, tc.y - h0 / 2), ImVec2(tc.x + w0 / 2, tc.y + h0 / 2)), true, 0.0f);
}

}  // namespace kk::gui

namespace kk::gui {

// The node editor drags blocks (and draws new links from points) with its drag button and has no
// other switch for it. Locked, the drag button is one the editor never presses its items with (it
// emits them for the select, navigate and context buttons only, 0 to 2), so a press on a block
// selects it and nothing moves. A double click on a block is then seen by the caller (the editor
// reports double clicks of the drag button).
void lock_canvas(ax::NodeEditor::EditorContext* editor, bool locked) {
  auto* ctx = reinterpret_cast<ax::NodeEditor::Detail::EditorContext*>(editor);
  if (ctx == nullptr) return;
  auto& cfg = const_cast<ax::NodeEditor::Detail::Config&>(ctx->GetConfig());  // the editor's own copy
  cfg.DragButtonIndex = locked ? ImGuiMouseButton_COUNT - 1 : ImGuiMouseButton_Left;
}

}  // namespace kk::gui
