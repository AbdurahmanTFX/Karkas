#include "kk/hsgm/width.hpp"

#include <fmt/format.h>

#include <cmath>
#include <set>
#include <vector>

namespace kk::hsgm {

namespace {

constexpr int kMinRuns = 5;
constexpr int kMinWidths = 2;

double cost_at(double c0, double c1, double b) { return (c0 + c1 * b) / std::log(b); }

}  // namespace

double optimal_width(double c0, double c1) {
  auto f = [&](double b) { return std::log(b) - 1.0 - c0 / (c1 * b); };
  double lo = 2.0, hi = 64.0;
  if (f(lo) >= 0) return lo;
  if (f(hi) <= 0) return hi;
  for (int k = 0; k < 100; ++k) {
    double mid = (lo + hi) / 2;
    (f(mid) < 0 ? lo : hi) = mid;
  }
  return (lo + hi) / 2;
}

Width width(const Input& in) {
  Width w;
  int groups = 0;
  long long children = 0;
  for (const auto& b : in.snap.blocks) {
    if (b.kind != "group" || b.children.empty()) continue;
    ++groups;
    children += static_cast<long long>(b.children.size());
  }
  w.mean = groups > 0 ? static_cast<double>(children) / groups : 0.0;

  struct Point {
    double b, usd, tokens;
  };
  std::vector<Point> pts;
  for (const auto& r : in.runs) {
    if (r.role != "navigator" || r.session_cost) continue;
    const auto* g = in.snap.block(r.block);
    if (g == nullptr || g->kind != "group" || g->children.empty()) continue;
    pts.push_back({static_cast<double>(g->children.size()), r.cost_usd,
                   static_cast<double>(r.input_tokens + r.cache_write_tokens + r.cache_read_tokens)});
  }
  w.runs = static_cast<int>(pts.size());
  std::set<double> distinct;
  bool all_usd = !pts.empty();
  for (const auto& p : pts) {
    distinct.insert(p.b);
    if (p.usd <= 0) all_usd = false;
  }
  w.widths = static_cast<int>(distinct.size());
  w.unit = all_usd ? "usd" : "tokens";
  w.enough = w.runs >= kMinRuns && w.widths >= kMinWidths;
  if (!w.enough) return w;

  double mb = 0, my = 0;
  for (const auto& p : pts) {
    mb += p.b;
    my += all_usd ? p.usd : p.tokens;
  }
  mb /= w.runs;
  my /= w.runs;
  double sbb = 0, sby = 0;
  for (const auto& p : pts) {
    double y = all_usd ? p.usd : p.tokens;
    sbb += (p.b - mb) * (p.b - mb);
    sby += (p.b - mb) * (y - my);
  }
  w.c1 = sby / sbb;
  w.c0 = my - w.c1 * mb;
  w.grows = w.c1 > 0;
  if (!w.grows) return w;
  w.best = optimal_width(w.c0, w.c1);
  if (w.mean >= 2) w.ratio = cost_at(w.c0, w.c1, w.mean) / cost_at(w.c0, w.c1, w.best);
  return w;
}

std::string format(const Width& w) {
  std::string out = fmt::format("@hsgm width runs={} widths={} unit={}\n", w.runs, w.widths, w.unit);
  if (!w.enough) {
    out += fmt::format("not enough data: {} runs, {} widths\n", w.runs, w.widths);
  } else if (!w.grows) {
    out += fmt::format("fit c0={:.6g} c1={:.6g}\n", w.c0, w.c1);
    out += "cost does not grow with width (c1 <= 0)\n";
  } else {
    out += fmt::format("fit c0={:.6g} c1={:.6g}\n", w.c0, w.c1);
    out += fmt::format("best {:.2f}  mean {:.2f}  cost at mean / best {}\n", w.best, w.mean,
                       w.ratio > 0 ? fmt::format("{:.3f}", w.ratio) : std::string("-"));
  }
  out += "assume one navigator run per level of a descent, its cost c0 + c1 × children\n";
  return out;
}

}  // namespace kk::hsgm
