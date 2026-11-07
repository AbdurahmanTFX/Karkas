// karkas test [<block>] — build and run tests without a model (8, 12).
#include <iostream>

#include "app.hpp"

namespace kk::cli {

bool have_test_pipeline() { return false; }
std::string run_tests_text(const fs::path&, const std::string&) { return "test pipeline not available\n"; }

}  // namespace kk::cli
