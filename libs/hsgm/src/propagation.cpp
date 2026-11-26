#include "kk/hsgm/propagation.hpp"

#include <fmt/format.h>

#include <algorithm>

#include "graph.hpp"
#include "text.hpp"

namespace kk::hsgm {

Propagation propagation(const Input& in) {
  Propagation p;
  const auto g = detail::dependency_graph(in);  // i -> j: i depends on j
  const std::size_t n = g.nodes.size();
  p.n = static_cast<int>(n);
  const auto v = detail::reach(g, true);  // visibility, the diagonal included (n = 0 of the sum)
  std::vector<int> affected(n, 0);        // column sums: who can be affected by a change to j
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      if (v[i][j] != 0) {
        ++p.transitive;
        ++affected[j];
      }
    }
  }
  p.direct = static_cast<long long>(n);
  for (const auto& out : g.out) p.direct += static_cast<long long>(out.size());
  for (std::size_t j = 0; j < n; ++j) p.reach.emplace_back(g.nodes[j], affected[j]);
  std::stable_sort(p.reach.begin(), p.reach.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  return p;
}

std::string to_text(const Propagation& p) {
  std::string out = fmt::format("@hsgm propagation blocks={}\n", p.n);
  if (p.n == 0) {
    out += "no code blocks\n";
  } else {
    const long long all = static_cast<long long>(p.n) * p.n;
    auto row = [&](const char* name, long long v) {
      return std::vector<std::string>{name, fmt::format("{}/{}", v, all), fmt::format("{:.2f}", static_cast<double>(v) / static_cast<double>(all))};
    };
    out += detail::table({row("transitive", p.transitive), row("direct", p.direct), row("self", p.n)});
    std::vector<std::vector<std::string>> top;
    for (std::size_t i = 0; i < p.reach.size() && i < 5; ++i) top.push_back({p.reach[i].first, std::to_string(p.reach[i].second)});
    out += "--- reach\n" + detail::table(top);
  }
  out += "assume a change reaches every block that depends on it through the schema's links (data, event, call, state, use)\n";
  out += "assume complete contracts stop a change at the direct dependents (direct) or at the block itself (self)\n";
  return out;
}

}  // namespace kk::hsgm
