#include "kk/agents/testing.hpp"

#include <fcntl.h>
#include <fmt/format.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <set>

#include "kk/code/gen.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

namespace {

// One build at a time per process (mutex) and per project (flock), 6.4.
std::mutex g_build_mu;

struct FileLock {
  int fd = -1;
  explicit FileLock(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fd = ::open(p.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd >= 0) ::flock(fd, LOCK_EX);
  }
  ~FileLock() {
    if (fd >= 0) {
      ::flock(fd, LOCK_UN);
      ::close(fd);
    }
  }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
};

}  // namespace

TestService::TestService(fs::path root, const core::Config& cfg) : root_(std::move(root)), cfg_(cfg) {}

std::string TestService::doctest_include() {
#ifdef KK_DOCTEST_INCLUDE_DIR
  return KK_DOCTEST_INCLUDE_DIR;
#else
  return {};
#endif
}

kk::Result<TestRun> TestService::build_and_test(const std::vector<std::string>& blocks, bool build_only) {
  std::lock_guard lk(g_build_mu);
  FileLock lock(root_ / ".karkas" / "build.lock");
  TestRun out;
  auto schema = core::load_schema(root_);
  if (!schema) return std::unexpected(schema.error());
  if (auto g = code::write_cmake(*schema); !g) return std::unexpected(g.error());
  auto build_dir = root_ / cfg_.build_dir;
  runner::BuildOptions bo;
  bo.source = root_;
  bo.build_dir = build_dir;
  bo.doctest_include = doctest_include();
  bo.timeout = std::chrono::seconds(cfg_.build_timeout_s);
  auto b = runner::build(bo);
  if (!b) return std::unexpected(b.error());
  out.build_ok = b->ok;
  out.build_summary = b->summary;
  index::StateStore store(root_);
  auto files = index::collect_code_files(*schema);
  if (!b->ok) {
    out.text = "build failed\n" + b->summary + "\n";
    std::vector<index::TestResult> rec;
    for (const auto& id : blocks) {
      const auto* blk = schema->find(id);
      if (blk == nullptr || !blk->has_code()) continue;
      index::TestResult r;
      r.block = id;
      r.code_hash = index::block_code_hash(files, id);
      r.build_failed = true;
      r.failures = core::split(b->summary, '\n');
      if (r.failures.size() > 10) r.failures.resize(10);
      r.ts = core::unix_now();
      rec.push_back(std::move(r));
    }
    if (!rec.empty()) (void)store.record_tests(rec);
    return out;
  }
  if (build_only) {
    out.ok = true;
    out.text = "build ok\n";
    return out;
  }
  runner::TestOptions to;
  to.source = root_;
  to.build_dir = build_dir;
  to.blocks = blocks;
  to.timeout = std::chrono::seconds(cfg_.test_timeout_s);
  to.probe_dir = root_ / ".karkas" / "probes";
  to.probe_run = fmt::format("test-{}", core::unix_now());
  auto t = runner::test(to);
  if (!t) return std::unexpected(t.error());
  out.blocks = t->blocks;
  out.ok = t->ok;
  out.text = t->text;
  std::vector<index::TestResult> rec;
  for (const auto& bt : t->blocks) {
    index::TestResult r;
    r.block = bt.block;
    r.code_hash = index::block_code_hash(files, bt.block);
    r.contract = {bt.contract.pass, bt.contract.total};
    r.unit = {bt.unit.pass, bt.unit.total};
    r.integration = {bt.integration.pass, bt.integration.total};
    r.build_failed = bt.build_failed;
    r.failures = bt.failures;
    r.ts = core::unix_now();
    rec.push_back(std::move(r));
  }
  if (!rec.empty()) {
    if (auto w = store.record_tests(rec); !w) return std::unexpected(w.error());
  }
  return out;
}

RegressionPlan regression_plan(const core::Schema& schema, const std::vector<std::string>& dependents,
                               const std::string& block, bool api_changed) {
  RegressionPlan p;
  std::set<std::string> labels = {block};
  for (const auto& a : schema.ancestors(block)) labels.insert(a);
  if (api_changed) {
    for (const auto& d : dependents) {
      labels.insert(d);
      for (const auto& a : schema.ancestors(d)) labels.insert(a);
      p.needs_review.push_back(d);
    }
  }
  p.labels.assign(labels.begin(), labels.end());
  return p;
}

}  // namespace kk::agents
