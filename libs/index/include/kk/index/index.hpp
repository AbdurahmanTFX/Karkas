#pragma once
// SQLite index (7.1): derived from cards, code and .karkas state; rebuilt at any time.
// One writer (update()), many readers. Queries (7.3) return compact KSL text for
// models or structured rows (Snapshot) for the GUI.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/error.hpp"
#include "kk/core/schema.hpp"
#include "kk/index/analyzer.hpp"
#include "kk/index/db.hpp"
#include "kk/index/state.hpp"

namespace kk::index {

namespace fs = std::filesystem;

// Bump when a table or the KSL checks change (ADR 0029: drift.col, meta service_fingerprint).
inline constexpr std::string_view kDbSchemaVersion = "3";

// --- structured rows ------------------------------------------------------------------
struct PortRow {
  std::string block, name, dir, type, signature;
  int line = 0;
};

struct LinkRow {
  std::int64_t id = 0;
  std::string src_block, src_port, dst_block, dst_port, kind, via, sync, rate;
  std::string thread_src, thread_dst, declared_in;
  int line = 0;
};

struct BlockRow {
  std::string id, parent, kind, purpose, thread, est, dir, status;
  int depth = 0;
  std::string card_hash, api_hash, api_frozen_hash;
  std::vector<std::string> children;  // in view order
  int failed_tests = 0;   // failing test cases (own; subtree for groups)
  int drift_errors = 0;   // E-level drift issues (own; subtree for groups)
  int impl_lines = 0;
};

struct DriftRow {
  std::string block, code, severity, message, file;
  int line = 0;
  [[nodiscard]] std::string str() const;  // "drift E D02 stream.pump impl/pump.cpp:12 message"
};

struct TicketRow {
  std::string id, kind, status, origin, current_block, title;
  std::int64_t created_at = 0, closed_at = 0;
};

struct ProbeRow {
  std::int64_t link_id = 0;
  std::string link, metric, run;
  std::int64_t n = 0;
  double min = 0, max = 0, avg = 0, last = 0;
  std::int64_t ts = 0;
  [[nodiscard]] std::string anomaly() const;  // "min0", "nonzero" or ""
  [[nodiscard]] std::string str() const;      // "a->b fill min=0 avg=3810 max=8192 anomaly=min0"
};

struct RunRow {
  std::int64_t id = 0;
  std::string ticket, role, block, provider, model, tier;
  int attempt = 0;
  std::int64_t input_tokens = 0, output_tokens = 0, cache_write_tokens = 0, cache_read_tokens = 0;
  std::int64_t cache_write_1h_tokens = 0;  // part of cache_write_tokens written with a 1-hour TTL (2x the input price)
  double cost_usd = 0;
  // The tokens and cost are those of the Claude Code session(s) Karkas launched for the run, at list prices
  // (.karkas/mcp/launch/costs.jsonl, docs/adr/0031), not the estimate from its turns.
  bool session_cost = false;
  std::int64_t duration_ms = 0;
  int steps = 0;
  std::string outcome;
  std::int64_t ts = 0;
  std::string journal;
};

struct TestRow {
  std::string block;
  TestCounts contract, unit, integration;
  bool build_failed = false;
  bool fresh = false;  // matches the current code
  std::vector<std::string> failures;
  std::int64_t ts = 0;  // when the tests ran
};

struct Snapshot {
  std::string project, idea;
  std::vector<BlockRow> blocks;  // sorted by id
  std::vector<PortRow> ports;
  std::vector<LinkRow> links;
  std::map<std::string, std::vector<std::string>> invariants;
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> history;  // block → (ticket, text)
  std::vector<DriftRow> drift;
  std::vector<TicketRow> tickets;
  std::vector<ProbeRow> probes;
  std::vector<TestRow> tests;
  std::vector<std::string> top_level;  // view order

  [[nodiscard]] const BlockRow* block(std::string_view id) const;
  [[nodiscard]] std::vector<const PortRow*> ports_of(std::string_view id) const;
  [[nodiscard]] const PortRow* port(std::string_view block, std::string_view name) const;
  [[nodiscard]] const TestRow* test(std::string_view block) const;
  [[nodiscard]] std::vector<std::string> children_of(std::string_view id) const;  // "" = top level
  [[nodiscard]] static bool under(std::string_view ancestor, std::string_view id);
};

struct UpdateOptions {
  bool full = false;                  // ignore fingerprints and re-analyze every file
  ICodeAnalyzer* analyzer = nullptr;  // nullptr = no code facts, no D checks
};

struct UpdateStats {
  bool changed = false;
  bool full = false;
  int blocks = 0;
  int files = 0;
  int analyzed = 0;
  double ms = 0;
  bool service_only = false;  // only tickets, changes, probes or run journals changed (ADR 0029)
};

struct ViewOptions {
  bool show_use = false;  // include use links and shared blocks on L0/L1 (3.7)
};

class Index {
 public:
  // Opens (and creates) <root>/.karkas/index.db.
  static kk::Result<std::unique_ptr<Index>> open(const fs::path& project_root);
  // Opens an index at an explicit path (tests).
  static kk::Result<std::unique_ptr<Index>> open_at(const fs::path& project_root, const fs::path& db_file);

  kk::Result<UpdateStats> update(const UpdateOptions& opts = {});

  [[nodiscard]] const fs::path& root() const { return root_; }
  Db& db() { return db_; }
  [[nodiscard]] Snapshot snapshot();
  // Last schema loaded by update() (empty before the first update).
  [[nodiscard]] const core::Schema* schema() const { return schema_ ? schema_.get() : nullptr; }

  // --- queries (7.3): compact KSL text -------------------------------------------------
  std::string overview(const ViewOptions& o = {});
  kk::Result<std::string> view(std::string_view block, int level, const ViewOptions& o = {});
  kk::Result<std::string> children(std::string_view block);
  kk::Result<std::string> neighbors(std::string_view block, std::string_view kind = "",
                                    std::string_view dir = "both");
  kk::Result<std::string> dependents(std::string_view block);
  kk::Result<std::string> path(std::string_view from, std::string_view to);
  kk::Result<std::string> writers(std::string_view portref);
  kk::Result<std::string> readers(std::string_view portref);
  std::string by_thread(std::string_view thread);
  std::string search(std::string_view text, int limit = 10);
  // Blocks that probably hold a symptom, ranked by its words found in block ids, purposes, file
  // names and code symbols (camelCase/snake_case split, light stemming); with files and the
  // matching symbols at file:line. One call instead of overview + view chains (docs/adr/0017).
  std::string locate(std::string_view text, int limit = 5);
  kk::Result<std::string> history(std::string_view block, int n = 10);
  kk::Result<std::string> status(std::string_view block = "");
  std::string probes(std::string_view target = "");
  std::string drift(std::string_view block = "");
  // Every issue of the last analysis — KSL checks and drift — in the order `karkas check` prints
  // them; with update() first, the answer of `karkas check` and kk_check (ADR 0029).
  std::vector<core::Issue> issues();

  // Structured helpers used by agents and the GUI.
  std::vector<std::string> dependent_blocks(std::string_view block);  // leaves depending on its api
  std::vector<RunRow> runs(std::string_view since_ts = "");
  kk::VoidResult insert_run(const RunRow& run);

 private:
  Index(fs::path root, Db db) : root_(std::move(root)), db_(std::move(db)) {}
  kk::VoidResult create_schema();
  fs::path root_;
  Db db_;
  std::unique_ptr<core::Schema> schema_;
  std::map<std::string, FileFacts> facts_cache_;  // by content hash
  bool facts_loaded_ = false;
  std::string last_fingerprint_;
  std::string last_service_fingerprint_;
};

// Card text for L2 views: canonical, without comments.
std::string card_text_without_comments(core::Block b);

// Code files of all blocks: api/impl/tests/mocks of leaf-like blocks and
// tests/integration/<group id>/ for groups, with xxh3 content hashes.
std::vector<CodeFile> collect_code_files(const core::Schema& schema);
// Hash of all code files of a block (freshness of test results).
std::string block_code_hash(const std::vector<CodeFile>& files, std::string_view block);
// Hash of what the test results of a block depend on: its own code files and the api/ of
// every block linked to it. A neighbor's contract change makes earlier results stale.
std::string block_test_hash(const core::Schema& schema, const std::vector<CodeFile>& files, std::string_view block);
// The same over the links computed once by the caller (a loop over every block; ADR 0028).
std::string block_test_hash(const std::vector<core::LinkInfo>& links, const std::vector<CodeFile>& files, std::string_view block);
// Hash of the block's api/ files ("" when it has none); compared with the frozen hash (D05).
std::string block_api_hash(const std::vector<CodeFile>& files, std::string_view block);
// Hash of the top-level structure (ids, kinds, purposes, lifted links) for "approve L0".
std::string l0_hash(const core::Schema& schema);

}  // namespace kk::index
