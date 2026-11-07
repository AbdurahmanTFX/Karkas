#pragma once
// Build and test without a model (karkas test, worker/tester tools, regression 12.4):
// regenerate CMake, configure, build, ctest by labels, record results for the index.
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "kk/core/config.hpp"
#include "kk/core/error.hpp"
#include "kk/core/schema.hpp"
#include "kk/runner/runner.hpp"

namespace kk::agents {

struct TestRun {
  bool build_ok = false;
  bool ok = false;
  std::string build_summary;  // compressed errors
  std::vector<runner::BlockTests> blocks;
  std::string text;  // compact summary for models
};

class ITestService {
 public:
  virtual ~ITestService() = default;
  // Builds everything (configure + build); `blocks` are ctest labels, empty = all tests.
  virtual kk::Result<TestRun> build_and_test(const std::vector<std::string>& blocks, bool build_only = false) = 0;
};

class TestService final : public ITestService {
 public:
  TestService(std::filesystem::path root, const core::Config& cfg);
  kk::Result<TestRun> build_and_test(const std::vector<std::string>& blocks, bool build_only = false) override;
  static std::string doctest_include();

 private:
  std::filesystem::path root_;
  const core::Config& cfg_;
};

// What to re-test after a change (12.4), computed from the schema.
struct RegressionPlan {
  std::vector<std::string> labels;        // ctest labels to run
  std::vector<std::string> needs_review;  // dependents whose contract changed
};
RegressionPlan regression_plan(const core::Schema& schema, const std::vector<std::string>& dependents,
                               const std::string& block, bool api_changed);

}  // namespace kk::agents
