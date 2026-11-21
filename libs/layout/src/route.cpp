// Orthogonal lines around blocks in a fixed picture (docs/adr/0044): the "all levels" view, where blocks sit
// inside the frames of their groups and no layered route exists. A Hanan grid of the blocks' edges (kept a
// margin away) and the middles of the gaps between them; a shortest path per line that pays for its
// length, its turns, the frames it crosses and the pieces other lines took already; then pieces that lie on
// one line are spread apart.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <tuple>

#include "kk/layout/layout.hpp"

namespace kk::layout {

namespace {

constexpr double kEps = 0.01;

Point dir_of(char side) {
  switch (side) {
    case 'L': return {-1, 0};
    case 'T': return {0, -1};
    case 'B': return {0, 1};
    default: return {1, 0};
  }
}

bool horizontal(char side) { return side == 'L' || side == 'R'; }

// Sorted coordinates without near repeats.
std::vector<double> uniq(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  std::vector<double> out;
  for (double x : v) {
    if (out.empty() || x - out.back() > 0.5) out.push_back(x);
  }
  return out;
}

std::size_t index_of(const std::vector<double>& v, double x) {
  auto it = std::lower_bound(v.begin(), v.end(), x - 0.5);
  return static_cast<std::size_t>(it - v.begin());
}

}  // namespace

std::vector<std::vector<Point>> route_around(const std::vector<Rect>& blocks, const std::vector<Rect>& frames, const std::vector<Wire>& wires,
                                            const RouteOptions& opt) {
  std::vector<std::vector<Point>> out(wires.size());
  if (wires.empty()) return out;
  const double m = opt.margin;
  std::vector<Rect> hard;  // blocks kept a margin away
  for (const auto& b : blocks) hard.push_back({b.x0 - m, b.y0 - m, b.x1 + m, b.y1 + m});
  // Where a line starts and ends: just outside its block, off its socket.
  auto stub = [&](const Port& p) {
    Point d = dir_of(p.side);
    return Point{p.at.x + d.x * (m + 0.5), p.at.y + d.y * (m + 0.5)};
  };
  std::vector<double> xs, ys;
  for (const auto& h : hard) {
    xs.insert(xs.end(), {h.x0, h.x1});
    ys.insert(ys.end(), {h.y0, h.y1});
  }
  for (const auto& f : frames) {  // lanes outside and inside a frame's border
    xs.insert(xs.end(), {f.x0 - m, f.x1 + m, f.x0 + m, f.x1 - m});
    ys.insert(ys.end(), {f.y0 - m, f.y1 + m, f.y0 + m, f.y1 - m});
  }
  for (const auto& w : wires) {
    for (const auto& p : {stub(w.from), stub(w.to)}) {
      xs.push_back(p.x);
      ys.push_back(p.y);
    }
  }
  auto with_middles = [&](std::vector<double> v) {  // lines run down the middle of a wide gap
    v = uniq(std::move(v));
    std::vector<double> mid;
    for (std::size_t i = 0; i + 1 < v.size(); ++i) {
      if (v[i + 1] - v[i] > 3 * m) mid.push_back((v[i] + v[i + 1]) / 2);
    }
    v.insert(v.end(), mid.begin(), mid.end());
    return uniq(std::move(v));
  };
  xs = with_middles(std::move(xs));
  ys = with_middles(std::move(ys));
  const std::size_t nx = xs.size();
  const std::size_t ny = ys.size();
  auto id = [&](std::size_t i, std::size_t j) { return i * ny + j; };
  auto inside = [&](double x, double y) {
    for (const auto& h : hard) {
      if (x > h.x0 + kEps && x < h.x1 - kEps && y > h.y0 + kEps && y < h.y1 - kEps) return true;
    }
    return false;
  };
  std::vector<char> free_node(nx * ny, 0);
  for (std::size_t i = 0; i < nx; ++i) {
    for (std::size_t j = 0; j < ny; ++j) free_node[id(i, j)] = inside(xs[i], ys[j]) ? 0 : 1;
  }
  // Pieces of the grid: to the right of (i, j) and below it; their cost of crossing frames.
  std::vector<char> h_ok(nx * ny, 0), v_ok(nx * ny, 0);
  std::vector<int> h_frames(nx * ny, 0), v_frames(nx * ny, 0);
  for (std::size_t i = 0; i < nx; ++i) {
    for (std::size_t j = 0; j < ny; ++j) {
      if (!free_node[id(i, j)]) continue;
      if (i + 1 < nx && free_node[id(i + 1, j)]) {
        bool ok = true;
        double y = ys[j];
        for (const auto& h : hard) {
          if (y > h.y0 + kEps && y < h.y1 - kEps && xs[i] < h.x1 - kEps && xs[i + 1] > h.x0 + kEps) ok = false;
        }
        h_ok[id(i, j)] = ok ? 1 : 0;
        for (const auto& f : frames) {
          if (y <= f.y0 || y >= f.y1) continue;
          for (double bx : {f.x0, f.x1}) h_frames[id(i, j)] += bx > xs[i] && bx < xs[i + 1] ? 1 : 0;
        }
      }
      if (j + 1 < ny && free_node[id(i, j + 1)]) {
        bool ok = true;
        double x = xs[i];
        for (const auto& h : hard) {
          if (x > h.x0 + kEps && x < h.x1 - kEps && ys[j] < h.y1 - kEps && ys[j + 1] > h.y0 + kEps) ok = false;
        }
        v_ok[id(i, j)] = ok ? 1 : 0;
        for (const auto& f : frames) {
          if (x <= f.x0 || x >= f.x1) continue;
          for (double by : {f.y0, f.y1}) v_frames[id(i, j)] += by > ys[j] && by < ys[j + 1] ? 1 : 0;
        }
      }
    }
  }
  std::vector<int> h_used(nx * ny, 0), v_used(nx * ny, 0);
  // --- a shortest path per line: states are (node, axis); turning costs `bend`.
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<double> dist(nx * ny * 2);
  std::vector<std::int64_t> prev(nx * ny * 2);
  for (std::size_t wi = 0; wi < wires.size(); ++wi) {
    const auto& w = wires[wi];
    Point s = stub(w.from);
    Point t = stub(w.to);
    std::size_t si = index_of(xs, s.x), sj = index_of(ys, s.y), ti = index_of(xs, t.x), tj = index_of(ys, t.y);
    if (si >= nx || sj >= ny || ti >= nx || tj >= ny || !free_node[id(si, sj)] || !free_node[id(ti, tj)]) continue;
    std::fill(dist.begin(), dist.end(), inf);
    std::fill(prev.begin(), prev.end(), -1);
    using Item = std::pair<double, std::size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
    const std::size_t start_axis = horizontal(w.from.side) ? 0 : 1;
    const std::size_t end_axis = horizontal(w.to.side) ? 0 : 1;
    dist[id(si, sj) * 2 + start_axis] = 0;
    heap.emplace(0.0, id(si, sj) * 2 + start_axis);
    std::size_t goal = id(ti, tj) * 2 + end_axis;
    while (!heap.empty()) {
      auto [d, st] = heap.top();
      heap.pop();
      if (d > dist[st]) continue;
      if (st == goal) break;
      std::size_t node = st / 2;
      std::size_t axis = st % 2;
      std::size_t i = node / ny;
      std::size_t j = node % ny;
      auto relax = [&](std::size_t ni, std::size_t nj, std::size_t nax, double len, int frames_crossed, int used) {
        double c = d + len * (1.0 + opt.shared * used) + (nax != axis ? opt.bend : 0.0) + opt.frame * frames_crossed;
        std::size_t ns = id(ni, nj) * 2 + nax;
        if (c < dist[ns]) {
          dist[ns] = c;
          prev[ns] = static_cast<std::int64_t>(st);
          heap.emplace(c, ns);
        }
      };
      if (i + 1 < nx && h_ok[id(i, j)]) relax(i + 1, j, 0, xs[i + 1] - xs[i], h_frames[id(i, j)], h_used[id(i, j)]);
      if (i > 0 && h_ok[id(i - 1, j)]) relax(i - 1, j, 0, xs[i] - xs[i - 1], h_frames[id(i - 1, j)], h_used[id(i - 1, j)]);
      if (j + 1 < ny && v_ok[id(i, j)]) relax(i, j + 1, 1, ys[j + 1] - ys[j], v_frames[id(i, j)], v_used[id(i, j)]);
      if (j > 0 && v_ok[id(i, j - 1)]) relax(i, j - 1, 1, ys[j] - ys[j - 1], v_frames[id(i, j - 1)], v_used[id(i, j - 1)]);
      // Arriving along the wrong axis: a turn on the spot, so the end can still enter its socket straight.
      if (node == id(ti, tj) && axis != end_axis && d + opt.bend < dist[goal]) {
        dist[goal] = d + opt.bend;
        prev[goal] = static_cast<std::int64_t>(st);
        heap.emplace(dist[goal], goal);
      }
    }
    if (dist[goal] == inf) continue;
    std::vector<std::size_t> nodes;
    for (std::int64_t st = static_cast<std::int64_t>(goal); st >= 0; st = prev[static_cast<std::size_t>(st)]) {
      std::size_t node = static_cast<std::size_t>(st) / 2;
      if (nodes.empty() || nodes.back() != node) nodes.push_back(node);
    }
    std::reverse(nodes.begin(), nodes.end());
    for (std::size_t k = 0; k + 1 < nodes.size(); ++k) {  // the next lines pay for these pieces
      std::size_t a = std::min(nodes[k], nodes[k + 1]);
      std::size_t b = std::max(nodes[k], nodes[k + 1]);
      if (b - a == ny) {
        ++h_used[a];
      } else {
        ++v_used[a];
      }
    }
    std::vector<Point> pts{w.from.at};
    for (auto node : nodes) pts.push_back({xs[node / ny], ys[node % ny]});
    pts.push_back(w.to.at);
    std::vector<Point> clean;
    for (const auto& p : pts) {
      if (!clean.empty() && std::fabs(clean.back().x - p.x) < kEps && std::fabs(clean.back().y - p.y) < kEps) continue;
      if (clean.size() >= 2) {
        const auto& u = clean[clean.size() - 2];
        const auto& v = clean.back();
        if ((std::fabs(u.x - v.x) < kEps && std::fabs(v.x - p.x) < kEps) || (std::fabs(u.y - v.y) < kEps && std::fabs(v.y - p.y) < kEps)) {
          clean.back() = p;
          continue;
        }
      }
      clean.push_back(p);
    }
    out[wi] = std::move(clean);
  }
  // --- pieces of different lines on one line, overlapping: spread apart, ordered by where their lines come
  //     from and go to, within the room the blocks around leave.
  struct Piece {
    std::size_t wire, k;  // points k and k + 1 of the wire
    double lo, hi, key;
  };
  for (int pass = 0; pass < 2; ++pass) {
    const bool level = pass == 0;  // level pieces (same y), then upright ones (same x)
    std::map<long, std::vector<Piece>> on_line;
    for (std::size_t wi = 0; wi < out.size(); ++wi) {
      const auto& p = out[wi];
      // Not the first and the last piece: they attach to the sockets (the pieces next to them turn, so
      // moving those keeps the ends level).
      for (std::size_t k = 1; k + 2 < p.size(); ++k) {
        const auto& a = p[k];
        const auto& b = p[k + 1];
        bool is_level = std::fabs(a.y - b.y) < kEps;
        if (is_level != level) continue;
        double c = level ? a.y : a.x;
        double lo = level ? std::min(a.x, b.x) : std::min(a.y, b.y);
        double hi = level ? std::max(a.x, b.x) : std::max(a.y, b.y);
        double key = level ? (p[k - 1].y + p[k + 2].y) / 2 : (p[k - 1].x + p[k + 2].x) / 2;
        on_line[std::lround(c * 2)].push_back({wi, k, lo, hi, key});
      }
    }
    for (auto& [ckey, pieces] : on_line) {
      std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return std::tie(a.lo, a.wire) < std::tie(b.lo, b.wire); });
      std::size_t first = 0;
      while (first < pieces.size()) {
        std::size_t last = first;
        double hi = pieces[first].hi;
        while (last + 1 < pieces.size() && pieces[last + 1].lo < hi - kEps) hi = std::max(hi, pieces[++last].hi);
        std::vector<Piece> group(pieces.begin() + static_cast<std::ptrdiff_t>(first), pieces.begin() + static_cast<std::ptrdiff_t>(last) + 1);
        first = last + 1;
        if (group.size() < 2) continue;
        double c = static_cast<double>(ckey) / 2.0;
        double lo = group.front().lo;
        double room = 4 * m;  // to the nearest block on either side of the line, along the pieces
        for (const auto& b : blocks) {
          double b_lo = level ? b.x0 : b.y0;
          double b_hi = level ? b.x1 : b.y1;
          if (b_hi < lo || b_lo > hi) continue;
          double before = level ? c - b.y1 : c - b.x1;
          double after = level ? b.y0 - c : b.x0 - c;
          if (before >= 0) room = std::min(room, before);
          if (after >= 0) room = std::min(room, after);
        }
        double span_room = room;
        double n = static_cast<double>(group.size());
        double step = std::min(opt.spacing, 2 * std::max(0.0, span_room - 4) / (n - 1));
        std::stable_sort(group.begin(), group.end(), [](const Piece& a, const Piece& b) { return a.key < b.key; });
        for (std::size_t r = 0; r < group.size(); ++r) {
          double off = (static_cast<double>(r) - (n - 1) / 2) * step;
          auto& p = out[group[r].wire];
          if (level) {
            p[group[r].k].y = c + off;
            p[group[r].k + 1].y = c + off;
          } else {
            p[group[r].k].x = c + off;
            p[group[r].k + 1].x = c + off;
          }
        }
      }
    }
  }
  return out;
}

}  // namespace kk::layout
