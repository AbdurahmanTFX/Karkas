#pragma once
// diag (ADR 0049): what the tests of a project can detect and tell apart, by group testing — a suite
// fails when at least one item it covers is broken (Aldridge, Johnson, Scarlett 2019, ch. 1–2).
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "kk/hsgm/input.hpp"

namespace kk::hsgm {

struct DiagItem {
  std::string id, kind;             // block id or link_name(); leaf|entry|shared or data|event|call|state
  bool link = false;
  std::vector<std::string> suites;  // the suites covering it (its column), sorted
  bool undetectable = false;        // no suite covers it
  bool everywhere = false;          // every suite covers it (at least two suites)
  bool alone = false;               // a non-empty column no other item has: one broken item is named by the failures
  int cls = 0;                      // 1-based number of its class (items with the same column), 0 = none
};

struct Diag {
  std::vector<std::string> suites;                          // "contract:<block>", "unit:<block>", "integration:<group>", sorted
  std::map<std::string, std::vector<std::string>> support;  // suite → the items it covers, in item order
  std::vector<DiagItem> items;                              // blocks by id, then links by id
  int k = 0;                  // largest k ≤ 2: up to k broken items among the detectable ones not in every suite are found exactly
  bool k_computed = true;     // false: too many items, the k = 2 pass was skipped
  int dominated = 0;          // items whose column is inside the column of one other item
  std::string witness;        // first item that breaks k + 1: "<i> inside <j>" or "<i> inside <j> + <l>"
  std::vector<std::string> passed, failed, unknown;           // suites by their fresh outcome
  std::vector<std::string> surely, possibly, cannot_rule_out;  // items (DD, COMP, undetectable)
};

inline constexpr int kMaxItemsForK = 400;

Diag diag(const Input& in);
std::string to_text(const Diag& d);

// Largest k in 0..max_k such that no column is inside the union of k other columns (k-disjunct).
// When a pass fails, `witness` gets (i, L) of its first violation in index order.
int max_disjunct(const std::vector<std::vector<int>>& cols, int max_k, std::pair<int, std::vector<int>>* witness);

}  // namespace kk::hsgm
