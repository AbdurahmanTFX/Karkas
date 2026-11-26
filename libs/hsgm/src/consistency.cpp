#include "kk/hsgm/consistency.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

#include "text.hpp"

namespace kk::hsgm {

namespace {

constexpr std::array<char, 3> kForms = {'a', 't', 'i'};  // api, tests, impl; the card is the anchor

// Bit sets over kForms: a = 1, t = 2, i = 4.
constexpr unsigned kA = 1, kT = 2, kI = 4;

std::string set_text(unsigned s) {
  std::string out = "{";
  for (std::size_t k = 0; k < kForms.size(); ++k) {
    if ((s & (1U << k)) == 0) continue;
    if (out.size() > 1) out += ", ";
    out += kForms[k];
  }
  return out + "}";
}

// Every minimal set of forms that meets each edge, by size, then by the order a < t < i.
std::vector<std::string> minimal_covers(const std::vector<unsigned>& edges) {
  std::vector<unsigned> covers;
  for (unsigned s = 1; s < 8; ++s) {
    if (std::all_of(edges.begin(), edges.end(), [&](unsigned e) { return (s & e) != 0; })) covers.push_back(s);
  }
  std::vector<unsigned> minimal;
  for (unsigned s : covers) {
    bool smaller = std::any_of(covers.begin(), covers.end(), [&](unsigned c) { return c != s && (c & s) == c; });
    if (!smaller) minimal.push_back(s);
  }
  auto key = [](unsigned s) {
    std::vector<int> ranks;
    for (int k = 0; k < 3; ++k) {
      if ((s & (1U << k)) != 0) ranks.push_back(k);
    }
    return ranks;
  };
  std::sort(minimal.begin(), minimal.end(), [&](unsigned x, unsigned y) {
    auto kx = key(x), ky = key(y);
    if (kx.size() != ky.size()) return kx.size() < ky.size();
    return kx < ky;
  });
  std::vector<std::string> out;
  for (unsigned s : minimal) out.push_back(set_text(s));
  return out;
}

}  // namespace

bool LeafForms::fails() const { return !ca.empty() || d08 || !untested.empty() || build || ti; }

std::string normalize_name(std::string_view s) {
  std::string out;
  bool space = false;
  for (char ch : s) {
    char c = ch == ';' ? ',' : ch;
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

Consistency consistency(const Input& in) {
  Consistency out;
  for (const auto& b : in.snap.blocks) {
    if (b.kind != "leaf" && b.kind != "entry") continue;
    ++out.checked;
    LeafForms f;
    f.block = b.id;
    std::set<std::string> ca;
    for (const auto& d : in.snap.drift) {
      if (d.block != b.id) continue;
      if (d.code == "D03" || d.code == "D04" || d.code == "D05") ca.insert(d.code);
      if (d.code == "D08") f.d08 = true;
    }
    f.ca.assign(ca.begin(), ca.end());
    std::set<std::string> names;
    if (auto it = in.test_cases.find(b.id); it != in.test_cases.end()) {
      for (const auto& n : it->second) names.insert(normalize_name(n));
    }
    if (auto it = in.snap.invariants.find(b.id); it != in.snap.invariants.end()) {
      for (const auto& inv : it->second) {
        if (!names.contains(normalize_name(inv))) f.untested.push_back(inv);
      }
    }
    if (const auto* row = in.snap.test(b.id); row != nullptr && row->fresh) {
      if (row->build_failed) {
        f.build = true;
      } else {
        f.ti = row->contract.pass < row->contract.total || row->unit.pass < row->unit.total;
      }
    }
    if (!f.fails()) continue;
    std::vector<unsigned> edges;
    if (!f.ca.empty()) edges.push_back(kA);
    if (f.d08 || !f.untested.empty()) edges.push_back(kT);
    if (f.build) edges.push_back(kA | kT | kI);
    if (f.ti) edges.push_back(kT | kI);
    f.regenerate = minimal_covers(edges);
    out.disagree.push_back(std::move(f));
  }
  return out;
}

std::string format(const Consistency& c) {
  std::string out = fmt::format("@hsgm consistency checked={} disagree={}\n", c.checked, c.disagree.size());
  for (const auto& f : c.disagree) {
    std::vector<std::string> edges;
    if (!f.ca.empty()) edges.push_back("c-a " + detail::capped(f.ca, " "));
    if (f.d08 || !f.untested.empty()) {
      std::string ct = "c-t";
      if (f.d08) ct += " D08";
      if (!f.untested.empty()) ct += fmt::format(" untested {}", f.untested.size());
      edges.push_back(ct);
    }
    if (f.build) edges.push_back("build");
    if (f.ti) edges.push_back("t-i");
    out += f.block + "  " + detail::capped(edges, "  ") + "  regenerate " + detail::capped(f.regenerate, " | ") + "\n";
    for (std::size_t k = 0; k < f.untested.size() && k < detail::kListCap; ++k) out += "  untested " + f.untested[k] + "\n";
    if (f.untested.size() > detail::kListCap) out += fmt::format("  … ({} more)\n", f.untested.size() - detail::kListCap);
  }
  out += "--- unchecked\n";
  out += "c-i: no check compares a card's invariants with its code\n";
  out += "assume the card is the anchor: it is never regenerated\n";
  out += "assume only fresh test results count\n";
  return out;
}

}  // namespace kk::hsgm
