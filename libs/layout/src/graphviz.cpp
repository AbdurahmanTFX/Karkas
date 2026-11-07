// Graphviz dot engine (14.2), compiled only when cgraph/gvc are found by pkg-config.
#include "kk/layout/layout.hpp"

#ifdef KK_HAVE_GRAPHVIZ
#include <graphviz/cgraph.h>
#include <graphviz/gvc.h>

#include <fmt/format.h>

#include <mutex>
#include <string>
#include <vector>
#endif

namespace kk::layout {

#ifdef KK_HAVE_GRAPHVIZ
namespace {

std::mutex g_gv_mu;  // gvc is not thread-safe

class GraphvizEngine final : public ILayoutEngine {
 public:
  kk::Result<Layout> layout(const Graph& g) override {
    std::lock_guard lk(g_gv_mu);
    Layout out;
    out.engine = name();
    GVC_t* gvc = gvContext();
    if (gvc == nullptr) return kk::fail("graphviz", "gvContext failed");
    char gname[] = "g";
    Agraph_t* graph = agopen(gname, Agdirected, nullptr);
    auto set = [](void* obj, const char* k, const std::string& v) {
      agsafeset(obj, const_cast<char*>(k), const_cast<char*>(v.c_str()), const_cast<char*>(""));
    };
    set(graph, "rankdir", "LR");
    set(graph, "nodesep", fmt::format("{:.3f}", g.node_sep / 72.0));
    set(graph, "ranksep", fmt::format("{:.3f}", g.rank_sep / 72.0));
    std::vector<std::pair<std::string, Agnode_t*>> nodes;
    for (const auto& n : g.nodes) {
      std::string id = n.id;
      Agnode_t* node = agnode(graph, id.data(), 1);
      set(node, "shape", "box");
      set(node, "fixedsize", "true");
      set(node, "width", fmt::format("{:.3f}", n.w / 72.0));
      set(node, "height", fmt::format("{:.3f}", n.h / 72.0));
      nodes.emplace_back(n.id, node);
    }
    auto find = [&](const std::string& id) -> Agnode_t* {
      for (const auto& [k, v] : nodes) {
        if (k == id) return v;
      }
      return nullptr;
    };
    for (const auto& e : g.edges) {
      Agnode_t* a = find(e.from);
      Agnode_t* b = find(e.to);
      if (a != nullptr && b != nullptr && a != b) agedge(graph, a, b, nullptr, 1);
    }
    if (gvLayout(gvc, graph, "dot") != 0) {
      agclose(graph);
      gvFreeContext(gvc);
      return kk::fail("graphviz", "dot layout failed; if Graphviz reports an unknown layout engine, run once: sudo dot -c");
    }
    double top = GD_bb(graph).UR.y;
    for (const auto& n : g.nodes) {
      Agnode_t* node = find(n.id);
      double cx = ND_coord(node).x;
      double cy = top - ND_coord(node).y;  // Graphviz y grows upwards
      Pos p{20 + cx - n.w / 2, 20 + cy - n.h / 2};
      out.pos[n.id] = p;
      out.width = std::max(out.width, p.x + n.w + 20);
      out.height = std::max(out.height, p.y + n.h + 20);
    }
    gvFreeLayout(gvc, graph);
    agclose(graph);
    gvFreeContext(gvc);
    return out;
  }
  [[nodiscard]] std::string name() const override { return "graphviz-dot"; }
};

}  // namespace

std::unique_ptr<ILayoutEngine> make_graphviz_engine() { return std::make_unique<GraphvizEngine>(); }
bool have_graphviz() { return true; }
#else
std::unique_ptr<ILayoutEngine> make_graphviz_engine() { return nullptr; }
bool have_graphviz() { return false; }
#endif

std::unique_ptr<ILayoutEngine> default_engine() {
  if (auto g = make_graphviz_engine()) return g;
  return std::make_unique<SugiyamaEngine>();
}

}  // namespace kk::layout
