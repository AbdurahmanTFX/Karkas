#pragma once
// Graph layout for the picture (14.2): only node positions are computed here; the GUI
// draws the links. Graphviz dot (rankdir=LR) when available, otherwise a built-in
// Sugiyama engine (docs/adr/0002). Layouts are cached by the hash of the level's
// structure; manual positions from .karkas/layout/ win over the automatic ones.
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kk/core/error.hpp"
#include "kk/index/index.hpp"

namespace kk::layout {

struct Node {
  std::string id;
  double w = 160;  // size in pixels (computed from the text width by the caller)
  double h = 48;
};

struct Edge {
  std::string from, to, kind;
};

struct Graph {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  double node_sep = 30;   // between nodes of one rank
  double rank_sep = 90;   // between ranks (left to right)
};

struct Pos {
  double x = 0, y = 0;  // top-left corner
};

struct Layout {
  std::map<std::string, Pos> pos;
  double width = 0, height = 0;
  std::string engine;
};

class ILayoutEngine {
 public:
  virtual ~ILayoutEngine() = default;
  virtual kk::Result<Layout> layout(const Graph& g) = 0;
  [[nodiscard]] virtual std::string name() const = 0;
};

// Layered layout (cycle removal, longest-path ranks, dummy nodes, barycentric crossing
// reduction, priority coordinate assignment). Deterministic: same graph → same layout.
class SugiyamaEngine final : public ILayoutEngine {
 public:
  kk::Result<Layout> layout(const Graph& g) override;
  [[nodiscard]] std::string name() const override { return "sugiyama"; }
};

// Graphviz dot through cgraph/gvc; nullptr when Karkas was built without Graphviz.
std::unique_ptr<ILayoutEngine> make_graphviz_engine();
// Graphviz if available, else Sugiyama.
std::unique_ptr<ILayoutEngine> default_engine();
bool have_graphviz();

std::string graph_hash(const Graph& g);

// The graph of one level of the schema (children of `scope`, lifted links, 3.6).
using Measure = std::function<std::pair<double, double>(const index::BlockRow&)>;
Graph level_graph(const index::Snapshot& snap, const std::string& scope, bool show_use, const Measure& measure);

// "All levels" (14.3): groups become boxes around their laid-out children.
struct Box {
  std::string id;
  double x = 0, y = 0, w = 0, h = 0;
  int depth = 0;
  bool group = false;
};
std::vector<Box> layout_all_levels(const index::Snapshot& snap, bool show_use, const Measure& measure, ILayoutEngine& engine);

// Cache (.karkas/cache/layout/<hash>.json) and manual positions (.karkas/layout/<view>.json).
class LayoutStore {
 public:
  explicit LayoutStore(std::filesystem::path project_root) : root_(std::move(project_root)) {}
  std::optional<Layout> cached(const std::string& hash) const;
  void store(const std::string& hash, const Layout& l) const;
  std::map<std::string, Pos> manual(const std::string& view) const;
  kk::VoidResult save_manual(const std::string& view, const std::map<std::string, Pos>& pos) const;
  // Computes (or loads) the layout and applies manual positions on top.
  Layout get(const std::string& view, const Graph& g, ILayoutEngine& engine) const;

 private:
  std::filesystem::path root_;
};

}  // namespace kk::layout
