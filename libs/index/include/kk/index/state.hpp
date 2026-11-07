#pragma once
// Persistent project state that the derived index must not own (5.4, 5.6):
//   .karkas/approvals.json   (in git)  approved L0, frozen api/ hashes, needs_review marks
//   .karkas/cache/tests.json (not git) last test results per block
// The index copies these into its tables; deleting index.db loses nothing.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "kk/core/error.hpp"

namespace kk::index {

namespace fs = std::filesystem;

struct TestCounts {
  int pass = 0;
  int total = 0;
  [[nodiscard]] bool ok() const { return pass == total; }
};

struct TestResult {
  std::string block;      // leaf id, or group id for integration tests
  std::string code_hash;  // hash of the block's code when the tests ran
  TestCounts contract, unit, integration;
  bool build_failed = false;
  std::vector<std::string> failures;  // short lines: "contract: never loses frames (ring_contract.cpp:41)"
  std::int64_t ts = 0;
  [[nodiscard]] int failed() const {
    return (contract.total - contract.pass) + (unit.total - unit.pass) + (integration.total - integration.pass);
  }
  [[nodiscard]] int total() const { return contract.total + unit.total + integration.total; }
};

struct ProjectState {
  std::string l0_hash;
  std::int64_t l0_at = 0;
  std::map<std::string, std::string> frozen_api;  // block → api hash at approval
  std::map<std::string, std::string> review;      // block → reason (needs_review)
  std::map<std::string, TestResult> tests;
};

// Creates .karkas/ with its sub-folders and .gitignore (5.4).
kk::VoidResult ensure_karkas_dir(const fs::path& project_root);
fs::path karkas_dir(const fs::path& project_root);

class StateStore {
 public:
  explicit StateStore(fs::path project_root) : root_(std::move(project_root)) {}
  [[nodiscard]] kk::Result<ProjectState> load() const;
  kk::VoidResult approve_l0(const std::string& hash);
  kk::VoidResult freeze_api(const std::string& block, const std::string& hash);
  kk::VoidResult unfreeze_api(const std::string& block);
  kk::VoidResult mark_review(const std::string& block, const std::string& reason);
  kk::VoidResult clear_review(const std::string& block);
  kk::VoidResult record_tests(const std::vector<TestResult>& results);
  [[nodiscard]] fs::path approvals_file() const;
  [[nodiscard]] fs::path tests_file() const;

 private:
  kk::VoidResult save_approvals(const ProjectState& s) const;
  kk::VoidResult save_tests(const ProjectState& s) const;
  fs::path root_;
};

}  // namespace kk::index
