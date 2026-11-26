#pragma once
// width (ADR 0049): the tree width that minimizes the navigator's cost. A descent through n leaves with b
// children per group takes log_b n levels at c0 + c1·b each: T(b) = (c0 + c1·b)·ln n / ln b, least at
// ln b = 1 + c0 / (c1·b). c0, c1 come from a least-squares fit over the project's navigator runs.
#include <string>

#include "kk/hsgm/input.hpp"

namespace kk::hsgm {

struct Width {
  int runs = 0, widths = 0;  // navigator runs used, distinct widths among them
  std::string unit;          // "usd" | "tokens"
  bool enough = false;       // at least 5 runs and 2 widths
  bool grows = false;        // c1 > 0
  double c0 = 0, c1 = 0;     // cost of one run = c0 + c1 · children of its group
  double best = 0;           // b*
  double mean = 0;           // mean children count of the project's groups (0 when none)
  double ratio = 0;          // T(mean) / T(best)
};

Width width(const Input& in);
std::string format(const Width& w);

// The root of ln b − 1 − c0 / (c1 b) on [2, 64] by bisection; 2 or 64 when it lies outside.
double optimal_width(double c0, double c1);

}  // namespace kk::hsgm
