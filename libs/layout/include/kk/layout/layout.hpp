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
  double w = 0, h = 0;  // manual size of the block on the picture; 0 = automatic
};

struct Point {
  double x = 0, y = 0;
};

// The path of a line between its two blocks (docs/adr/0042): the points it passes through, in the
// layout's coordinates — across the column of every rank it passes over, between the blocks there.
// Empty: the line joins neighbor ranks, a plain curve will do.
struct Route {
  std::string from, to, kind;
  std::vector<Point> points;
};

struct Layout {
  std::map<std::string, Pos> pos;
  double width = 0, height = 0;
  std::string engine;
  std::vector<Route> routes;  // one per edge of the graph (the built-in engine; not cached)
};

class ILayoutEngine {
 public:
  virtual ~ILayoutEngine() = default;
  virtual kk::Result<Layout> layout(const Graph& g) = 0;
  [[nodiscard]] virtual std::string name() const = 0;
};

// Layered layout (docs/adr/0042): a pair linked both ways is one edge; cycles broken by the greedy
// order of Eades, Lin and Smyth; ranks by the longest path, then pulled to shorten lines; dummy nodes on
// long lines; order by median sweeps and transposition (crossings counted with a Fenwick tree);
// positions per rank by weighted isotonic regression (long lines straight); routes through the dummies;
// connected parts packed on shelves, single blocks in rows. Deterministic: same graph → same layout.
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

// Children of `scope`, where the groups `expand` says yes to are opened in place: their
// children (recursively, by the same rule) are laid out inside them and the opened group
// becomes a frame (Box::group = true); other blocks stay single boxes. Boxes are absolute
// in the level's coordinates, frames before their contents. Frame padding: kFramePad on the
// sides and the bottom, kFrameTitle above the contents.
inline constexpr double kFramePad = 14;
inline constexpr double kFrameTitle = 34;
std::vector<Box> layout_nested(const index::Snapshot& snap, const std::string& scope,
                               const std::function<bool(const std::string&)>& expand, bool show_use,
                               const Measure& measure, ILayoutEngine& engine);

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
