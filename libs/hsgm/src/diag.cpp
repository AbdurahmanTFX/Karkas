#include "kk/hsgm/diag.hpp"

#include <algorithm>
#include <cstdint>
#include <set>

namespace kk::hsgm {

namespace {

bool item_link_kind(std::string_view k) { return k == "data" || k == "event" || k == "call" || k == "state"; }

// Suite of a test file (ADR 0049): tests/integration/<group>/ → integration, a contract/ folder → contract,
// anything else under a block's tests/ → unit.
std::string suite_of(const index::CodeFile& f) {
  if (f.path.starts_with("tests/integration/")) return "integration:" + f.block;
  if (f.path.find("/contract/") != std::string::npos) return "contract:" + f.block;
  return "unit:" + f.block;
}

struct Bits {
  std::vector<std::uint64_t> w;
  explicit Bits(int n) : w(static_cast<std::size_t>((n + 63) / 64), 0) {}
  void set(int i) { w[static_cast<std::size_t>(i) / 64] |= std::uint64_t{1} << (static_cast<unsigned>(i) % 64U); }
};

// a ⊆ b
bool inside(const Bits& a, const Bits& b) {
  for (std::size_t i = 0; i < a.w.size(); ++i) {
    if ((a.w[i] & ~b.w[i]) != 0) return false;
  }
  return true;
}

// a ⊆ b ∪ c
bool inside(const Bits& a, const Bits& b, const Bits& c) {
  for (std::size_t i = 0; i < a.w.size(); ++i) {
    if ((a.w[i] & ~(b.w[i] | c.w[i])) != 0) return false;
  }
  return true;
}

}  // namespace

int max_disjunct(const std::vector<std::vector<int>>& cols, int max_k, std::pair<int, std::vector<int>>* witness) {
  max_k = std::min(max_k, 2);
  if (max_k <= 0) return 0;
  const std::size_t n = cols.size();
  int universe = 0;
  for (const auto& c : cols) {
    for (int v : c) universe = std::max(universe, v + 1);
  }
  std::vector<Bits> bits;
  bits.reserve(cols.size());
  for (const auto& c : cols) {
    Bits b(universe);
    for (int v : c) b.set(v);
    bits.push_back(std::move(b));
  }
  auto as_int = [](std::size_t v) { return static_cast<int>(v); };
  // k = 1: no column inside one other column.
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      if (j != i && inside(bits[i], bits[j])) {
        if (witness != nullptr) *witness = {as_int(i), {as_int(j)}};
        return 0;
      }
    }
  }
  if (max_k < 2) return 1;
  // k = 2: no column inside the union of two others.
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      if (j == i) continue;
      for (std::size_t l = j + 1; l < n; ++l) {
        if (l != i && inside(bits[i], bits[j], bits[l])) {
          if (witness != nullptr) *witness = {as_int(i), {as_int(j), as_int(l)}};
          return 1;
        }
      }
    }
  }
  return 2;
}

Diag diag(const Input& in) {
  Diag d;
  const auto& snap = in.snap;

  std::set<std::string> code;
  for (const auto& b : snap.blocks) {
    if (is_code_block(b)) code.insert(b.id);
  }
  std::map<std::string, std::vector<std::string>> uses;  // user → shared blocks it uses
  for (const auto& l : snap.links) {
    if (l.kind == "use" && code.contains(l.src_block) && code.contains(l.dst_block)) uses[l.src_block].push_back(l.dst_block);
  }
  auto add_used = [&](const std::string& v, std::set<std::string>& out) {
    std::vector<std::string> stack{v};
    while (!stack.empty()) {
      auto x = stack.back();
      stack.pop_back();
      auto it = uses.find(x);
      if (it == uses.end()) continue;
      for (const auto& y : it->second) {
        if (out.insert(y).second) stack.push_back(y);
      }
    }
  };

  // Items: code blocks (snapshot order = by id), then links between code blocks by name.
  std::vector<DiagItem> items;
  for (const auto& b : snap.blocks) {
    if (code.contains(b.id)) items.push_back({.id = b.id, .kind = b.kind});
  }
  struct LinkItem {
    std::string id;
    const index::LinkRow* row;
  };
  std::vector<LinkItem> links;
  for (const auto& l : snap.links) {
    if (item_link_kind(l.kind) && code.contains(l.src_block) && code.contains(l.dst_block)) links.push_back({link_name(l), &l});
  }
  std::stable_sort(links.begin(), links.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  std::map<std::string, int> seen;  // the same link declared twice: "… #2"
  for (auto& li : links) {
    if (int n = ++seen[li.id]; n > 1) li.id += " #" + std::to_string(n);
  }
  const std::size_t first_link = items.size();
  for (const auto& li : links) items.push_back({.id = li.id, .kind = li.row->kind, .link = true});
  std::map<std::string, std::size_t> at;
  for (std::size_t k = 0; k < items.size(); ++k) at[items[k].id] = k;

  // Suites from the test files.
  std::set<std::string> suites;
  for (const auto& f : in.files) {
    if (f.role != "test" || f.block.empty()) continue;
    auto s = suite_of(f);
    bool known = s.starts_with("integration:") ? snap.block(f.block) != nullptr : code.contains(f.block);
    if (known) suites.insert(s);
  }
  d.suites.assign(suites.begin(), suites.end());

  // Supports and columns.
  for (const auto& s : d.suites) {
    auto colon = s.find(':');
    std::string kind = s.substr(0, colon);
    std::string block = s.substr(colon + 1);
    std::set<std::string> blocks;
    if (kind == "integration") {
      for (const auto& c : code) {
        if (index::Snapshot::under(block, c)) blocks.insert(c);
      }
    } else {
      blocks.insert(block);
    }
    std::set<std::string> covered = blocks;
    for (const auto& b : blocks) add_used(b, covered);
    std::vector<std::size_t> member;
    for (const auto& b : covered) member.push_back(at.at(b));
    if (kind == "integration") {
      for (std::size_t k = 0; k < links.size(); ++k) {
        if (blocks.contains(links[k].row->src_block) && blocks.contains(links[k].row->dst_block)) member.push_back(first_link + k);
      }
    }
    std::sort(member.begin(), member.end());
    auto& sup = d.support[s];
    for (auto k : member) {
      sup.push_back(items[k].id);
      items[k].suites.push_back(s);  // suites run in sorted order, so every column is sorted
    }
  }

  // Per item: undetectable, everywhere, alone, class.
  std::map<std::vector<std::string>, int> same;  // column → how many items have it
  for (auto& it : items) {
    it.undetectable = it.suites.empty();
    it.everywhere = d.suites.size() >= 2 && it.suites.size() == d.suites.size();
    if (!it.undetectable) ++same[it.suites];
  }
  std::map<std::vector<std::string>, int> class_of;
  for (auto& it : items) {
    if (it.undetectable) continue;
    if (same[it.suites] == 1) {
      it.alone = true;
      continue;
    }
    auto [pos, added] = class_of.try_emplace(it.suites, static_cast<int>(class_of.size()) + 1);
    it.cls = pos->second;
  }

  // Disjunctness over the detectable items that are not in every suite.
  std::map<std::string, int> suite_at;
  for (std::size_t i = 0; i < d.suites.size(); ++i) suite_at[d.suites[i]] = static_cast<int>(i);
  std::vector<std::size_t> det;
  std::vector<std::vector<int>> cols;
  for (std::size_t k = 0; k < items.size(); ++k) {
    if (items[k].undetectable || items[k].everywhere) continue;
    det.push_back(k);
    std::vector<int> c;
    for (const auto& s : items[k].suites) c.push_back(suite_at.at(s));
    cols.push_back(std::move(c));
  }
  if (det.size() <= 1) {
    d.k = 2;
  } else {
    int max_k = 2;
    if (det.size() > static_cast<std::size_t>(kMaxItemsForK)) {
      max_k = 1;
      d.k_computed = false;
    }
    std::pair<int, std::vector<int>> w{-1, {}};
    d.k = max_disjunct(cols, max_k, &w);
    if (d.k < max_k && w.first >= 0) {
      d.witness = items[det[static_cast<std::size_t>(w.first)]].id + " inside " + items[det[static_cast<std::size_t>(w.second[0])]].id;
      if (w.second.size() > 1) d.witness += " + " + items[det[static_cast<std::size_t>(w.second[1])]].id;
    }
  }
  for (std::size_t a = 0; a < det.size(); ++a) {
    for (std::size_t b = 0; b < det.size(); ++b) {
      if (a != b && std::includes(cols[b].begin(), cols[b].end(), cols[a].begin(), cols[a].end())) {
        ++d.dominated;
        break;
      }
    }
  }

  // Fresh outcomes of the suites.
  for (const auto& s : d.suites) {
    auto colon = s.find(':');
    std::string kind = s.substr(0, colon);
    const auto* row = snap.test(s.substr(colon + 1));
    if (row == nullptr || !row->fresh || row->build_failed) {
      d.unknown.push_back(s);
      continue;
    }
    const auto& c = kind == "contract" ? row->contract : kind == "unit" ? row->unit : row->integration;
    if (c.total == 0) {
      d.unknown.push_back(s);
    } else if (c.pass == c.total) {
      d.passed.push_back(s);
    } else {
      d.failed.push_back(s);
    }
  }

  // COMP: whatever no passed suite covers may be broken; DD: the only such item of a failed suite is broken.
  std::set<std::string> passed(d.passed.begin(), d.passed.end());
  std::set<std::string> failed(d.failed.begin(), d.failed.end());
  for (const auto& it : items) {
    if (it.undetectable) {
      d.cannot_rule_out.push_back(it.id);
    } else if (std::none_of(it.suites.begin(), it.suites.end(), [&](const auto& s) { return passed.contains(s); })) {
      d.possibly.push_back(it.id);
    }
  }
  std::set<std::string> possible(d.possibly.begin(), d.possibly.end());
  for (const auto& it : items) {
    if (!possible.contains(it.id)) continue;
    for (const auto& s : it.suites) {
      if (!failed.contains(s)) continue;
      const auto& sup = d.support.at(s);
      if (std::count_if(sup.begin(), sup.end(), [&](const auto& id) { return possible.contains(id); }) == 1) {
        d.surely.push_back(it.id);
        break;
      }
    }
  }

  d.items = std::move(items);
  return d;
}

}  // namespace kk::hsgm
