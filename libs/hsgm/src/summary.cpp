#include "kk/hsgm/summary.hpp"

#include <fmt/format.h>

#include "kk/hsgm/consistency.hpp"
#include "kk/hsgm/diag.hpp"
#include "kk/hsgm/propagation.hpp"
#include "kk/hsgm/views.hpp"
#include "kk/hsgm/width.hpp"

namespace kk::hsgm {

namespace {

std::string line(std::string_view name, const std::string& text) { return fmt::format("{:<13}{}\n", name, text); }

}  // namespace

std::string summary(const Input& in) {
  std::string out = "@hsgm summary\n";

  auto d = diag(in);
  int blocks = 0, blocks_alone = 0, links = 0, links_alone = 0, links_class = 0, links_none = 0;
  for (const auto& i : d.items) {
    if (i.link) {
      ++links;
      links_alone += i.alone ? 1 : 0;
      links_class += i.cls > 0 ? 1 : 0;
      links_none += i.undetectable ? 1 : 0;
    } else {
      ++blocks;
      blocks_alone += i.alone ? 1 : 0;
    }
  }
  std::string k = d.k_items <= 1 ? "k 2 (vacuous)" : fmt::format("k {}", d.k);
  if (!d.k_computed) k += " (k 2 not checked)";
  out += line("diag", fmt::format("blocks alone {}/{}, links alone {}/{} ({} in classes, {} undetectable), {}", blocks_alone, blocks,
                                  links_alone, links, links_class, links_none, k));

  auto v = views(in);
  if (v.levels.empty()) {
    out += line("views", "nothing lifted");
  } else {
    const auto& l = v.levels.front();
    out += line("views", fmt::format("level 0 precision {}/{}, false cycles {}", l.true_pairs, l.pairs, l.false_cycles.size()));
  }

  auto p = propagation(in);
  if (p.n == 0) {
    out += line("propagation", "no code blocks");
  } else {
    const double all = static_cast<double>(p.n) * p.n;
    out += line("propagation", fmt::format("transitive {:.2f}, direct {:.2f}, self {:.2f}", static_cast<double>(p.transitive) / all,
                                           static_cast<double>(p.direct) / all, p.n / all));
  }

  auto c = consistency(in);
  out += line("consistency", fmt::format("{} of {} disagree", c.disagree.size(), c.checked));

  auto w = to_text(width(in));
  auto first = w.find('\n');
  auto second = first == std::string::npos ? std::string::npos : w.find('\n', first + 1);
  out += line("width", second == std::string::npos ? std::string() : w.substr(first + 1, second - first - 1));
  out += "assume every line rests on the assumptions of its measurement: karkas hsgm <measurement> prints them\n";
  return out;
}

}  // namespace kk::hsgm
