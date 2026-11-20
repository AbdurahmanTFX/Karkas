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
  std::string text;            // compact summary for models
  std::string build_warnings;  // compressed compiler warnings of the files this build compiled
};

class ITestService {
 public:
  virtual ~ITestService() = default;
  // Builds everything (configure + build); `blocks` are ctest labels, empty = all tests.
  virtual kk::Result<TestRun> build_and_test(const std::vector<std::string>& blocks, bool build_only = false) = 0;
  // Compiles the given files (relative to the project root) without linking: seconds instead of the
  // project's full build (ADR 0028). The default builds the project.
  virtual kk::Result<TestRun> compile_check(const std::vector<std::string>& files) {
    (void)files;
    return build_and_test({}, true);
  }
};

class TestService final : public ITestService {
 public:
  TestService(std::filesystem::path root, const core::Config& cfg);
  kk::Result<TestRun> build_and_test(const std::vector<std::string>& blocks, bool build_only = false) override;
  // A project with its own build and a compile_commands.json: each file's compile command with
  // -fsyntax-only (no object, no link); otherwise the build.
  kk::Result<TestRun> compile_check(const std::vector<std::string>& files) override;
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
