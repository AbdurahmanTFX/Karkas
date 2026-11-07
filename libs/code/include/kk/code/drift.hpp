#pragma once
// Stand-alone drift check (used by `karkas check`): analyzes every code file and
// returns D01–D09 issues.
#include <vector>

#include "kk/core/schema.hpp"

namespace kk::code {

std::vector<core::Issue> check_drift(const core::Schema& schema);

}  // namespace kk::code
