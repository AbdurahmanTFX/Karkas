#pragma once
// consistency (ADR 0049): where the forms of a leaf disagree — card c (the anchor: the human approves
// it), api a, tests t, impl i — and which forms to regenerate: every minimal set of forms that touches
// each failed edge (a vertex cover with the anchor fixed).
#include <string>
#include <string_view>
#include <vector>

#include "kk/hsgm/input.hpp"

namespace kk::hsgm {

struct LeafForms {
  std::string block;
  std::vector<std::string> ca;        // c–a: D03, D04, D05 found on the block, sorted, unique
  bool d08 = false;                   // c–t: D08 (a leaf without contract tests)
  std::vector<std::string> untested;  // c–t: inv with no contract TEST_CASE of the same normalized name
  bool build = false;                 // {a, t, i}: fresh results with the build failed
  bool ti = false;                    // t–i: fresh results with contract or unit failures
  std::vector<std::string> regenerate;  // minimal sets over a, t, i (a < t < i), by size then order: "{a, t}"
  [[nodiscard]] bool fails() const;
};

struct Consistency {
  int checked = 0;                  // leaf and entry blocks
  std::vector<LeafForms> disagree;  // those with a failed edge, by id
};

Consistency consistency(const Input& in);
std::string format(const Consistency& c);

// Test name ↔ invariant: whitespace runs as one space, trimmed, ';' read as ',' (CMake cuts test names at ';').
std::string normalize_name(std::string_view s);

}  // namespace kk::hsgm
