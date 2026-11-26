#pragma once
// Test helpers of kk_hsgm: an Input made by hand.
#include <cstdint>
#include <string>

#include "kk/hsgm/input.hpp"

namespace kk::hsgm::test {

struct Make {
  Input in;
  // Parent and depth come from the id; a parent must be added before its children.
  Make& block(const std::string& id, const std::string& kind);
  Make& link(const std::string& kind, const std::string& src, const std::string& dst,
             const std::string& src_port = "", const std::string& dst_port = "");
  // A test file of `block`: kind "contract" | "unit" | "integration" (then `block` is a group).
  Make& suite(const std::string& block, const std::string& kind);
  Make& result(const std::string& block, index::TestCounts contract, index::TestCounts unit = {},
               index::TestCounts integration = {}, bool fresh = true, bool build_failed = false);
  Make& drift(const std::string& block, const std::string& code);
  Make& inv(const std::string& block, const std::string& text);
  Make& test_case(const std::string& block, const std::string& name);
  Make& run(const std::string& role, const std::string& block, double cost_usd, std::int64_t input_tokens = 0,
            bool session_cost = false);
};

}  // namespace kk::hsgm::test
