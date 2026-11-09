#include "kk/index/index.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <set>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/index/hash.hpp"

namespace kk::index {

using json = nlohmann::json;

namespace {

constexpr std::string_view kDdl = R"SQL(
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS blocks(
  id TEXT PRIMARY KEY, parent TEXT, kind TEXT NOT NULL,
  purpose TEXT NOT NULL, thread TEXT, est TEXT,
  depth INTEGER NOT NULL, dir TEXT NOT NULL,
  card_hash TEXT NOT NULL, api_hash TEXT, api_frozen_hash TEXT,
  status TEXT NOT NULL DEFAULT 'design', updated_at INTEGER);
CREATE TABLE IF NOT EXISTS ports(
  block TEXT NOT NULL REFERENCES blocks(id) ON DELETE CASCADE,
  name TEXT NOT NULL, dir TEXT NOT NULL,
  type TEXT, signature TEXT, line INTEGER,
  PRIMARY KEY(block, name));
CREATE TABLE IF NOT EXISTS links(
  id INTEGER PRIMARY KEY,
  src_block TEXT NOT NULL, src_port TEXT,
  dst_block TEXT NOT NULL, dst_port TEXT,
  kind TEXT NOT NULL,
  via TEXT, sync TEXT, rate TEXT,
  thread_src TEXT, thread_dst TEXT,
  declared_in TEXT NOT NULL, line INTEGER);
CREATE TABLE IF NOT EXISTS invariants(block TEXT, idx INTEGER, text TEXT, PRIMARY KEY(block, idx));
CREATE TABLE IF NOT EXISTS history(block TEXT, ticket TEXT, summary TEXT, ts INTEGER);
CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY, block TEXT, role TEXT, hash TEXT, lines INTEGER);
CREATE TABLE IF NOT EXISTS symbols(id INTEGER PRIMARY KEY, file TEXT, block TEXT,
  name TEXT, kind TEXT, line INTEGER, exported INTEGER);
CREATE TABLE IF NOT EXISTS code_refs(from_block TEXT, from_file TEXT, line INTEGER,
  to_block TEXT, to_role TEXT, kind TEXT);
CREATE TABLE IF NOT EXISTS drift(block TEXT, code TEXT, severity TEXT,
  message TEXT, file TEXT, line INTEGER);
CREATE TABLE IF NOT EXISTS tickets(id TEXT PRIMARY KEY, kind TEXT, status TEXT,
  origin TEXT, current_block TEXT, created_at INTEGER, closed_at INTEGER, title TEXT);
CREATE TABLE IF NOT EXISTS runs(id INTEGER PRIMARY KEY, ticket TEXT, role TEXT,
  block TEXT, provider TEXT, model TEXT, tier TEXT, attempt INTEGER,
  input_tokens INTEGER, output_tokens INTEGER,
  cache_write_tokens INTEGER, cache_read_tokens INTEGER,
  cost_usd REAL, duration_ms INTEGER, steps INTEGER,
  outcome TEXT, ts INTEGER, journal TEXT UNIQUE);
CREATE TABLE IF NOT EXISTS probes(link_id INTEGER, metric TEXT, n INTEGER,
  min REAL, max REAL, avg REAL, last REAL, run TEXT, ts INTEGER, link TEXT);
CREATE VIRTUAL TABLE IF NOT EXISTS search USING fts5(block, purpose, inv, hist);
CREATE TABLE IF NOT EXISTS test_results(block TEXT PRIMARY KEY,
  contract_pass INTEGER, contract_total INTEGER, unit_pass INTEGER, unit_total INTEGER,
  integration_pass INTEGER, integration_total INTEGER, build_failed INTEGER,
  fresh INTEGER, failures TEXT, ts INTEGER);
CREATE TABLE IF NOT EXISTS facts(hash TEXT PRIMARY KEY, json TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS links_src ON links(src_block);
CREATE INDEX IF NOT EXISTS links_dst ON links(dst_block);
CREATE INDEX IF NOT EXISTS blocks_parent ON blocks(parent);
CREATE INDEX IF NOT EXISTS files_block ON files(block);
CREATE INDEX IF NOT EXISTS code_refs_from ON code_refs(from_block);
CREATE INDEX IF NOT EXISTS runs_ticket ON runs(ticket);
CREATE INDEX IF NOT EXISTS runs_block ON runs(block);
)SQL";

constexpr std::string_view kSkeletonMarker = "karkas:skeleton";

json facts_to_json(const FileFacts& f) {
  json syms = json::array();
  for (const auto& s : f.symbols) {
    syms.push_back(json{{"n", s.name}, {"k", s.kind}, {"l", s.line}, {"e", s.exported}, {"p", s.params},
                        {"s", s.scope}, {"v", s.pure_virtual}, {"g", s.signature}, {"a", s.access}});
  }
  json refs = json::array();
  for (const auto& r : f.refs) {
    refs.push_back(json{{"l", r.line}, {"k", r.kind}, {"t", r.target}, {"y", r.system}});
  }
  return json{{"symbols", syms}, {"refs", refs}};
}

FileFacts facts_from_json(const json& j) {
  FileFacts f;
  for (const auto& s : j.value("symbols", json::array())) {
    f.symbols.push_back(SymbolFact{s.value("n", ""), s.value("k", ""), s.value("l", 0), s.value("e", false),
                                   s.value("p", -1), s.value("s", ""), s.value("v", false), s.value("g", ""),
                                   s.value("a", "")});
  }
  for (const auto& r : j.value("refs", json::array())) {
    f.refs.push_back(RefFact{r.value("l", 0), r.value("k", ""), r.value("t", ""), r.value("y", false)});
  }
  return f;
}

std::string role_of(const core::Dirs& dirs, const fs::path& rel_in_block) {
  auto first = rel_in_block.begin() != rel_in_block.end() ? rel_in_block.begin()->string() : "";
  if (first == dirs.api) return "api";
  if (first == dirs.impl) return "impl";
  if (first == dirs.tests) return "test";
  if (first == dirs.mocks) return "mock";
  return "other";
}

void add_files_under(const core::Schema& schema, const fs::path& dir, const std::string& block,
                     const std::function<std::string(const fs::path&)>& role, std::vector<CodeFile>& out) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> paths;
  for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& p = it->path();
    if (it->is_directory(ec) && p.filename().string().starts_with(".")) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec) && core::is_code_file(p)) paths.push_back(p);
  }
  std::sort(paths.begin(), paths.end());
  for (const auto& p : paths) {
    auto text = core::read_file(p);
    if (!text) continue;
    CodeFile f;
    f.path = core::rel_str(schema.root, p);
    f.block = block;
    f.role = role(p);
    f.hash = xxh3_hex(*text);
    f.lines = static_cast<int>(std::count(text->begin(), text->end(), '\n'));
    f.skeleton = text->find(kSkeletonMarker) != std::string::npos;
    out.push_back(std::move(f));
  }
}

bool is_header(const fs::path& p) {
  auto ext = p.extension().string();
  return ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx" || ext == ".ipp" || ext == ".inl";
}

// Files listed by "files" of an imported block: headers are its api, the rest its impl.
void add_listed_files(const core::Schema& schema, const core::Block& b, std::vector<CodeFile>& out) {
  for (const auto& rel : b.file_list()) {
    auto p = schema.root / rel;
    auto text = core::read_file(p);
    if (!text) continue;  // E09 reports it
    CodeFile f;
    f.path = rel;
    f.block = b.id;
    f.role = is_header(p) ? "api" : "impl";
    f.hash = xxh3_hex(*text);
    f.lines = static_cast<int>(std::count(text->begin(), text->end(), '\n'));
    f.skeleton = text->find(kSkeletonMarker) != std::string::npos;
    out.push_back(std::move(f));
  }
}

std::string file_fingerprint(const fs::path& p) {
  std::error_code ec;
  if (!fs::exists(p, ec)) return "-";
  auto text = core::read_file(p);
  return text ? xxh3_hex(*text) : "?";
}

// Names + sizes + mtimes of the files in a folder (cheap change detector).
std::string dir_fingerprint(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return "-";
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    auto t = fs::last_write_time(e.path(), ec).time_since_epoch().count();
    items.emplace_back(e.path().filename().string(), fmt::format("{}:{}", e.file_size(ec), t));
  }
  return combine_hashes(std::move(items));
}

std::int64_t mtime_s(const fs::path& p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  if (ec) return 0;
  auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
  return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

core::BlockStatus compute_status(const core::Block& b, const std::vector<const CodeFile*>& files,
                                 const ProjectState& st, bool drift_error, const std::string& api_hash,
                                 const std::string& code_hash) {
  using core::BlockStatus;
  if (drift_error) return BlockStatus::Drift;
  if (st.review.contains(b.id)) return BlockStatus::NeedsReview;
  if (auto it = st.tests.find(b.id); it != st.tests.end() && it->second.code_hash == code_hash) {
    const auto& r = it->second;
    if (r.build_failed || r.failed() > 0) return BlockStatus::Failing;
    if (r.total() > 0) return BlockStatus::Passing;
  }
  bool impl = false;
  bool contract = false;
  for (const auto* f : files) {
    if (f->role == "impl" && !f->skeleton) impl = true;
    if (f->role == "test" && f->path.find("/" + std::string("contract") + "/") != std::string::npos) contract = true;
  }
  if (impl) return BlockStatus::Implemented;
  // Imported code (header-only modules included): it exists, so it is implemented.
  if (b.external() && std::any_of(files.begin(), files.end(), [](const CodeFile* f) { return f->role == "api" && !f->skeleton; })) {
    return BlockStatus::Implemented;
  }
  if (contract) return BlockStatus::TestsReady;
  auto fz = st.frozen_api.find(b.id);
  if (fz != st.frozen_api.end() && !api_hash.empty() && fz->second == api_hash) return BlockStatus::ApiFrozen;
  return BlockStatus::Design;
}

}  // namespace

// --- free helpers --------------------------------------------------------------------------

std::vector<CodeFile> collect_code_files(const core::Schema& schema) {
  std::vector<CodeFile> out;
  const auto& dirs = schema.project.dirs;
  for (const auto& b : schema.blocks) {
    if (!b.has_code()) continue;
    auto dir = schema.root / b.dir;
    add_files_under(schema, dir, b.id, [&](const fs::path& p) { return role_of(dirs, p.lexically_relative(dir)); },
                    out);
    if (b.external()) add_listed_files(schema, b, out);
  }
  // Integration tests of groups: tests/integration/<group id>/ (12.3).
  auto integ = schema.root / "tests" / "integration";
  std::error_code ec;
  if (fs::is_directory(integ, ec)) {
    for (const auto& e : fs::directory_iterator(integ, ec)) {
      if (!e.is_directory(ec)) continue;
      std::string id = e.path().filename().string();
      add_files_under(schema, e.path(), id, [](const fs::path&) { return std::string("test"); }, out);
    }
  }
  std::sort(out.begin(), out.end(), [](const CodeFile& a, const CodeFile& b) { return a.path < b.path; });
  return out;
}

std::string block_code_hash(const std::vector<CodeFile>& files, std::string_view block) {
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto& f : files) {
    if (f.block == block) items.emplace_back(f.path, f.hash);
  }
  return combine_hashes(std::move(items));
}

std::string block_api_hash(const std::vector<CodeFile>& files, std::string_view block) {
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto& f : files) {
    if (f.block == block && f.role == "api") items.emplace_back(f.path, f.hash);
  }
  if (items.empty()) return {};
  return combine_hashes(std::move(items));
}

std::string l0_hash(const core::Schema& schema) {
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto* b : schema.top_level()) {
    items.emplace_back(b->id, std::string(b->kind_name()) + "|" + b->purpose.value);
  }
  for (const auto& l : schema.links()) {
    auto top = [](const std::string& id) { return core::split(id, '.').front(); };
    auto a = top(l.src_block);
    auto c = top(l.dst_block);
    if (a != c) items.emplace_back("link:" + a + "->" + c, std::string(core::to_string(l.kind)));
  }
  return combine_hashes(std::move(items));
}

std::string card_text_without_comments(core::Block b) {
  core::Trivia none;
  b.header = none;
  b.tail.clear();
  for (auto* s : {&b.purpose, &b.kind_text, &b.thread, &b.est}) s->trivia = none;
  for (auto& p : b.ports) p.trivia = none;
  for (auto& c : b.calls) c.trivia = none;
  for (auto& u : b.uses) u.trivia = none;
  for (auto& s : b.states) s.trivia = none;
  for (auto& i : b.invs) i.trivia = none;
  for (auto& n : b.notes) n.trivia = none;
  for (auto& h : b.hist) h.trivia = none;
  return core::write_block(b);
}

std::string DriftRow::str() const {
  std::string loc = file;
  if (line > 0) loc += fmt::format(":{}", line);
  return fmt::format("drift {} {} {}{}{}", severity, code, block.empty() ? "-" : block,
                     loc.empty() ? "" : " " + loc, " " + message);
}

std::string ProbeRow::anomaly() const {
  auto m = core::to_lower(metric);
  if (core::contains(m, "fill") && n > 0 && min <= 0.0 && max > 0.0) return "min0";
  if ((core::contains(m, "drop") || core::contains(m, "overrun") || core::contains(m, "underrun") ||
       core::contains(m, "error")) &&
      max > 0.0) {
    return "nonzero";
  }
  return {};
}

std::string ProbeRow::str() const {
  auto num = [](double v) {
    if (v == static_cast<double>(static_cast<long long>(v))) return fmt::format("{}", static_cast<long long>(v));
    return fmt::format("{:.3g}", v);
  };
  std::string s = fmt::format("{} {} min={} avg={} max={}", link, metric, num(min), num(avg), num(max));
  auto a = anomaly();
  if (!a.empty()) s += " anomaly=" + a;
  return s;
}

// --- Snapshot helpers ----------------------------------------------------------------------

const BlockRow* Snapshot::block(std::string_view id) const {
  auto it = std::lower_bound(blocks.begin(), blocks.end(), id,
                             [](const BlockRow& b, std::string_view v) { return b.id < v; });
  return (it != blocks.end() && it->id == id) ? &*it : nullptr;
}

std::vector<const PortRow*> Snapshot::ports_of(std::string_view id) const {
  std::vector<const PortRow*> out;
  for (const auto& p : ports) {
    if (p.block == id) out.push_back(&p);
  }
  return out;
}

const PortRow* Snapshot::port(std::string_view b, std::string_view name) const {
  for (const auto& p : ports) {
    if (p.block == b && p.name == name) return &p;
  }
  return nullptr;
}

const TestRow* Snapshot::test(std::string_view b) const {
  for (const auto& t : tests) {
    if (t.block == b) return &t;
  }
  return nullptr;
}

std::vector<std::string> Snapshot::children_of(std::string_view id) const {
  if (id.empty() || id == "root") return top_level;
  const auto* b = block(id);
  return b != nullptr ? b->children : std::vector<std::string>{};
}

bool Snapshot::under(std::string_view ancestor, std::string_view id) { return core::Schema::is_under(ancestor, id); }

// --- Index ----------------------------------------------------------------------------------

kk::Result<std::unique_ptr<Index>> Index::open(const fs::path& project_root) {
  auto root = core::normalize(project_root);
  if (auto r = ensure_karkas_dir(root); !r) return std::unexpected(r.error());
  return open_at(root, karkas_dir(root) / "index.db");
}

kk::Result<std::unique_ptr<Index>> Index::open_at(const fs::path& project_root, const fs::path& db_file) {
  auto db = Db::open(db_file);
  if (!db) return std::unexpected(db.error());
  std::unique_ptr<Index> idx(new Index(core::normalize(project_root), std::move(*db)));
  if (auto r = idx->create_schema(); !r) return std::unexpected(r.error());
  return idx;
}

kk::VoidResult Index::create_schema() {
  if (auto r = db_.exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=NORMAL;"); !r) return r;
  // Recreate the derived database when its layout version changed.
  bool has_meta = false;
  {
    auto s = db_.q("SELECT name FROM sqlite_master WHERE type='table' AND name='meta'");
    has_meta = s.step();
  }
  if (has_meta) {
    auto s = db_.q("SELECT value FROM meta WHERE key='schema_version'");
    std::string v = s.step() ? s.text(0) : "";
    if (v != kDbSchemaVersion) {
      spdlog::info("index: schema version {} -> {}, rebuilding", v, kDbSchemaVersion);
      std::vector<std::string> tables;
      auto t = db_.q("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' AND name NOT LIKE 'search_%'");
      while (t.step()) tables.push_back(t.text(0));
      for (const auto& name : tables) {
        if (auto r = db_.exec("DROP TABLE IF EXISTS \"" + name + "\""); !r) return r;
      }
    }
  }
  if (auto r = db_.exec(kDdl); !r) return r;
  auto s = db_.q("INSERT OR REPLACE INTO meta(key, value) VALUES('schema_version', ?)");
  s.bind(1, kDbSchemaVersion);
  return s.run();
}

kk::Result<UpdateStats> Index::update(const UpdateOptions& opts) {
  auto t0 = std::chrono::steady_clock::now();
  UpdateStats stats;
  stats.full = opts.full;

  auto schema = core::load_schema(root_);
  if (!schema) return std::unexpected(schema.error());
  StateStore store(root_);
  auto state = store.load();
  if (!state) return std::unexpected(state.error());
  auto files = collect_code_files(*schema);
  stats.blocks = static_cast<int>(schema->blocks.size());
  stats.files = static_cast<int>(files.size());

  // Fingerprint of everything the index derives from.
  std::vector<std::pair<std::string, std::string>> fp;
  fp.emplace_back("project.ksl", file_fingerprint(root_ / "project.ksl"));
  for (const auto& b : schema->blocks) fp.emplace_back("card:" + b.id, file_fingerprint(root_ / b.dir / "block.ksl"));
  for (const auto& i : schema->issues) fp.emplace_back("issue:" + i.str(), "");
  for (const auto& f : files) fp.emplace_back("file:" + f.path, f.hash + f.role + f.block);
  auto kdir = karkas_dir(root_);
  fp.emplace_back("approvals", file_fingerprint(store.approvals_file()));
  fp.emplace_back("tests", file_fingerprint(store.tests_file()));
  fp.emplace_back("tickets", dir_fingerprint(kdir / "tickets"));
  fp.emplace_back("changes", dir_fingerprint(kdir / "changes"));
  fp.emplace_back("probes", dir_fingerprint(kdir / "probes"));
  fp.emplace_back("runs", dir_fingerprint(kdir / "runs"));
  fp.emplace_back("cmake", file_fingerprint(root_ / "CMakeLists.txt"));
  fp.emplace_back("analyzer", opts.analyzer != nullptr ? "yes" : "no");
  fp.emplace_back("version", std::string(kDbSchemaVersion));
  std::string fingerprint = combine_hashes(fp);
  if (last_fingerprint_.empty()) {
    auto s = db_.q("SELECT value FROM meta WHERE key='fingerprint'");
    if (s.step()) last_fingerprint_ = s.text(0);
  }
  schema_ = std::make_unique<core::Schema>(*schema);
  if (!opts.full && fingerprint == last_fingerprint_) {
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return stats;
  }
  stats.changed = true;

  // Code facts, cached by content hash.
  if (!facts_loaded_) {
    auto s = db_.q("SELECT hash, json FROM facts");
    while (s.step()) {
      try {
        facts_cache_[s.text(0)] = facts_from_json(json::parse(s.text(1)));
      } catch (const json::exception&) {
      }
    }
    facts_loaded_ = true;
  }
  IndexFacts facts;
  facts.state = &*state;
  std::vector<std::pair<std::string, FileFacts>> new_facts;
  if (opts.analyzer != nullptr) {
    for (const auto& f : files) {
      auto it = facts_cache_.find(f.hash);
      if (it == facts_cache_.end() || opts.full) {
        auto text = core::read_file(root_ / f.path);
        if (!text) continue;
        auto ff = opts.analyzer->analyze(f, *text);
        facts_cache_[f.hash] = ff;
        new_facts.emplace_back(f.hash, ff);
        ++stats.analyzed;
        it = facts_cache_.find(f.hash);
      }
      facts.facts[f.path] = it->second;
    }
  }
  facts.files = files;
  for (const auto& b : schema->blocks) {
    if (b.has_code()) facts.api_hash[b.id] = block_api_hash(files, b.id);
  }

  // Checks: KSL (E/W/S) + drift (D).
  auto issues = core::check_schema(*schema);
  CheckOutput check_out;
  if (opts.analyzer != nullptr) check_out = opts.analyzer->check(*schema, facts);
  issues.insert(issues.end(), check_out.issues.begin(), check_out.issues.end());

  if (auto r = db_.begin(); !r) return std::unexpected(r.error());
  auto fail_tx = [&](const kk::Error& e) -> kk::Result<UpdateStats> {
    db_.rollback();
    return std::unexpected(e);
  };
  for (const char* t : {"DELETE FROM blocks", "DELETE FROM ports", "DELETE FROM links", "DELETE FROM invariants",
                        "DELETE FROM history", "DELETE FROM files", "DELETE FROM symbols", "DELETE FROM code_refs",
                        "DELETE FROM drift", "DELETE FROM search", "DELETE FROM tickets", "DELETE FROM probes",
                        "DELETE FROM test_results"}) {
    if (auto r = db_.exec(t); !r) return fail_tx(r.error());
  }
  for (const auto& [h, ff] : new_facts) {
    auto s = db_.q("INSERT OR REPLACE INTO facts(hash, json) VALUES(?, ?)");
    s.bind(1, h).bind(2, facts_to_json(ff).dump());
    if (auto r = s.run(); !r) return fail_tx(r.error());
  }

  std::set<std::string> drift_blocks;
  for (const auto& i : issues) {
    if (i.severity == 'E') drift_blocks.insert(i.block);
  }

  auto now = core::unix_now();
  auto ins_block = db_.q(
      "INSERT INTO blocks(id, parent, kind, purpose, thread, est, depth, dir, card_hash, api_hash, api_frozen_hash, "
      "status, updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
  auto ins_port = db_.q("INSERT OR IGNORE INTO ports(block, name, dir, type, signature, line) VALUES(?,?,?,?,?,?)");
  auto ins_inv = db_.q("INSERT OR REPLACE INTO invariants(block, idx, text) VALUES(?,?,?)");
  auto ins_hist = db_.q("INSERT INTO history(block, ticket, summary, ts) VALUES(?,?,?,?)");
  auto ins_search = db_.q("INSERT INTO search(block, purpose, inv, hist) VALUES(?,?,?,?)");
  std::map<std::string, std::vector<const CodeFile*>> by_block;
  for (const auto& f : files) by_block[f.block].push_back(&f);
  for (const auto& b : schema->blocks) {
    std::string api_h = b.has_code() ? facts.api_hash[b.id] : "";
    std::string frozen;
    if (auto it = state->frozen_api.find(b.id); it != state->frozen_api.end()) frozen = it->second;
    auto code_h = block_code_hash(files, b.id);
    auto status = b.has_code() ? compute_status(b, by_block[b.id], *state, drift_blocks.contains(b.id), api_h, code_h)
                               : (drift_blocks.contains(b.id) ? core::BlockStatus::Drift : core::BlockStatus::Design);
    ins_block.bind(1, b.id)
        .bind_opt(2, b.parent)
        .bind(3, b.kind ? std::string(core::to_string(*b.kind)) : std::string("?"))
        .bind(4, b.purpose.value)
        .bind_opt(5, b.thread.value)
        .bind_opt(6, b.est.value)
        .bind(7, b.depth)
        .bind(8, b.dir)
        .bind(9, file_fingerprint(root_ / b.dir / "block.ksl"))
        .bind_opt(10, api_h)
        .bind_opt(11, frozen)
        .bind(12, core::to_string(status))
        .bind(13, now);
    if (auto r = ins_block.run(); !r) return fail_tx(r.error());
    for (const auto& p : b.ports) {
      ins_port.bind(1, b.id).bind(2, p.name).bind(3, core::to_string(p.dir));
      if (p.dir == core::PortDir::Fn) {
        ins_port.bind_opt(4, p.ret).bind(5, p.signature());
      } else {
        ins_port.bind(4, p.type).bind(5, p.signature());
      }
      ins_port.bind(6, p.line);
      if (auto r = ins_port.run(); !r) return fail_tx(r.error());
    }
    std::vector<std::string> invs;
    for (std::size_t i = 0; i < b.invs.size(); ++i) {
      ins_inv.bind(1, b.id).bind(2, static_cast<int>(i)).bind(3, b.invs[i].text);
      if (auto r = ins_inv.run(); !r) return fail_tx(r.error());
      invs.push_back(b.invs[i].text);
    }
    std::vector<std::string> hist;
    for (std::size_t i = 0; i < b.hist.size(); ++i) {
      ins_hist.bind(1, b.id).bind(2, b.hist[i].ticket).bind(3, b.hist[i].text).bind(4, static_cast<int>(i));
      if (auto r = ins_hist.run(); !r) return fail_tx(r.error());
      hist.push_back(b.hist[i].ticket + " " + b.hist[i].text);
    }
    ins_search.bind(1, b.id).bind(2, b.purpose.value).bind(3, core::join(invs, "; ")).bind(4, core::join(hist, "; "));
    if (auto r = ins_search.run(); !r) return fail_tx(r.error());
  }

  auto ins_link = db_.q(
      "INSERT INTO links(src_block, src_port, dst_block, dst_port, kind, via, sync, rate, thread_src, thread_dst, "
      "declared_in, line) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
  for (const auto& l : schema->links()) {
    ins_link.bind(1, l.src_block)
        .bind_opt(2, l.src_port)
        .bind(3, l.dst_block)
        .bind_opt(4, l.dst_port)
        .bind(5, core::to_string(l.kind))
        .bind_opt(6, l.via)
        .bind_opt(7, l.sync)
        .bind_opt(8, l.rate)
        .bind_opt(9, l.thread_src)
        .bind_opt(10, l.thread_dst)
        .bind(11, l.declared_in)
        .bind(12, l.line);
    if (auto r = ins_link.run(); !r) return fail_tx(r.error());
  }

  auto ins_file = db_.q("INSERT OR REPLACE INTO files(path, block, role, hash, lines) VALUES(?,?,?,?,?)");
  auto ins_sym = db_.q("INSERT INTO symbols(file, block, name, kind, line, exported) VALUES(?,?,?,?,?,?)");
  for (const auto& f : files) {
    ins_file.bind(1, f.path).bind_opt(2, f.block).bind(3, f.role).bind(4, f.hash).bind(5, f.lines);
    if (auto r = ins_file.run(); !r) return fail_tx(r.error());
    auto it = facts.facts.find(f.path);
    if (it == facts.facts.end()) continue;
    for (const auto& s : it->second.symbols) {
      ins_sym.bind(1, f.path).bind_opt(2, f.block).bind(3, s.name).bind(4, s.kind).bind(5, s.line).bind(6, s.exported ? 1 : 0);
      if (auto r = ins_sym.run(); !r) return fail_tx(r.error());
    }
  }
  auto ins_ref = db_.q("INSERT INTO code_refs(from_block, from_file, line, to_block, to_role, kind) VALUES(?,?,?,?,?,?)");
  for (const auto& r : check_out.refs) {
    ins_ref.bind_opt(1, r.from_block).bind(2, r.from_file).bind(3, r.line).bind_opt(4, r.to_block).bind_opt(5, r.to_role).bind(6, r.kind);
    if (auto rr = ins_ref.run(); !rr) return fail_tx(rr.error());
  }
  auto ins_drift = db_.q("INSERT INTO drift(block, code, severity, message, file, line) VALUES(?,?,?,?,?,?)");
  for (const auto& i : issues) {
    ins_drift.bind_opt(1, i.block).bind(2, i.code).bind(3, std::string(1, i.severity)).bind(4, i.message)
        .bind_opt(5, i.loc.file).bind(6, i.loc.line);
    if (auto r = ins_drift.run(); !r) return fail_tx(r.error());
  }
  auto ins_test = db_.q(
      "INSERT OR REPLACE INTO test_results(block, contract_pass, contract_total, unit_pass, unit_total, "
      "integration_pass, integration_total, build_failed, fresh, failures, ts) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
  for (const auto& [id, t] : state->tests) {
    bool fresh = t.code_hash == block_code_hash(files, id);
    ins_test.bind(1, id).bind(2, t.contract.pass).bind(3, t.contract.total).bind(4, t.unit.pass)
        .bind(5, t.unit.total).bind(6, t.integration.pass).bind(7, t.integration.total)
        .bind(8, t.build_failed ? 1 : 0).bind(9, fresh ? 1 : 0).bind(10, core::join(t.failures, "\n")).bind(11, t.ts);
    if (auto r = ins_test.run(); !r) return fail_tx(r.error());
  }

  // Tickets.
  {
    auto ins = db_.q("INSERT OR REPLACE INTO tickets(id, kind, status, origin, current_block, created_at, closed_at, "
                     "title) VALUES(?,?,?,?,?,?,?,?)");
    std::error_code ec;
    std::vector<fs::path> tfiles;
    for (const auto& e : fs::directory_iterator(kdir / "tickets", ec)) {
      if (e.path().extension() == ".ksl") tfiles.push_back(e.path());
    }
    std::sort(tfiles.begin(), tfiles.end());
    for (const auto& p : tfiles) {
      auto text = core::read_file(p);
      if (!text) continue;
      auto t = core::parse_ticket_text(*text, core::rel_str(root_, p));
      if (!t) {
        spdlog::warn("index: {}", t.error().str());
        continue;
      }
      std::string cur;
      for (const auto& e : t->trail) {
        if (e.type != core::TrailEntry::Type::Level) continue;
        cur = e.get("pick").empty() ? e.get("at") : e.get("pick");
      }
      auto mt = mtime_s(p);
      ins.bind(1, t->id).bind(2, t->kind.value).bind(3, t->status.value).bind(4, t->origin.value).bind_opt(5, cur)
          .bind(6, mt).bind(7, t->status.value == "closed" ? mt : 0)
          .bind(8, t->symptom.set ? t->symptom.value : t->task.value);
      if (auto r = ins.run(); !r) return fail_tx(r.error());
    }
  }

  // Probes: fold raw JSONL into summaries (12.5).
  {
    std::map<std::pair<std::string, std::string>, std::int64_t> link_ids;
    {
      auto s = db_.q("SELECT id, src_block, dst_block FROM links");
      while (s.step()) link_ids.emplace(std::make_pair(s.text(1), s.text(2)), s.i64(0));
    }
    auto ins = db_.q("INSERT INTO probes(link_id, metric, n, min, max, avg, last, run, ts, link) VALUES(?,?,?,?,?,?,?,?,?,?)");
    std::error_code ec;
    std::vector<fs::path> pfiles;
    for (const auto& e : fs::directory_iterator(kdir / "probes", ec)) {
      if (e.path().extension() == ".jsonl") pfiles.push_back(e.path());
    }
    std::sort(pfiles.begin(), pfiles.end());
    for (const auto& p : pfiles) {
      auto text = core::read_file(p);
      if (!text) continue;
      struct Agg {
        std::int64_t n = 0;
        double min = 0, max = 0, sum = 0, last = 0;
      };
      std::map<std::pair<std::string, std::string>, Agg> agg;
      for (const auto& line : core::split(*text, '\n')) {
        try {
          auto j = json::parse(line);
          std::string link = j.value("link", "");
          std::string metric = j.value("metric", "");
          double v = j.value("v", 0.0);
          auto& a = agg[{link, metric}];
          if (a.n == 0) a.min = a.max = v;
          a.min = std::min(a.min, v);
          a.max = std::max(a.max, v);
          a.sum += v;
          a.last = v;
          ++a.n;
        } catch (const json::exception&) {
        }
      }
      auto run = p.stem().string();
      auto ts = mtime_s(p);
      for (const auto& [key, a] : agg) {
        std::int64_t lid = 0;
        auto arrow = key.first.find("->");
        if (arrow != std::string::npos) {
          auto it = link_ids.find({key.first.substr(0, arrow), key.first.substr(arrow + 2)});
          if (it != link_ids.end()) lid = it->second;
        }
        ins.bind(1, lid).bind(2, key.second).bind(3, a.n).bind(4, a.min).bind(5, a.max)
            .bind(6, a.n > 0 ? a.sum / static_cast<double>(a.n) : 0.0).bind(7, a.last).bind(8, run).bind(9, ts)
            .bind(10, key.first);
        if (auto r = ins.run(); !r) return fail_tx(r.error());
      }
    }
  }

  // Runs: load journals that are not in the table yet (16.1).
  {
    std::set<std::string> known;
    {
      auto s = db_.q("SELECT journal FROM runs WHERE journal IS NOT NULL");
      while (s.step()) known.insert(s.text(0));
    }
    std::error_code ec;
    std::vector<fs::path> jfiles;
    for (const auto& e : fs::directory_iterator(kdir / "runs", ec)) {
      if (e.is_regular_file(ec) && e.path().extension() == ".jsonl") jfiles.push_back(e.path());
    }
    std::sort(jfiles.begin(), jfiles.end());
    for (const auto& p : jfiles) {
      auto name = p.filename().string();
      if (known.contains(name)) continue;
      auto text = core::read_file(p);
      if (!text) continue;
      auto lines = core::split(*text, '\n');
      for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        try {
          auto j = json::parse(*it);
          if (j.value("type", "") != "run_end") continue;
          RunRow r;
          r.ticket = j.value("ticket", "");
          r.role = j.value("role", "");
          r.block = j.value("block", "");
          r.provider = j.value("provider", "");
          r.model = j.value("model", "");
          r.tier = j.value("tier", "");
          r.attempt = j.value("attempt", 0);
          r.input_tokens = j.value("input_tokens", std::int64_t{0});
          r.output_tokens = j.value("output_tokens", std::int64_t{0});
          r.cache_write_tokens = j.value("cache_write_tokens", std::int64_t{0});
          r.cache_read_tokens = j.value("cache_read_tokens", std::int64_t{0});
          r.cost_usd = j.value("cost_usd", 0.0);
          r.duration_ms = j.value("duration_ms", std::int64_t{0});
          r.steps = j.value("steps", 0);
          r.outcome = j.value("outcome", "");
          r.ts = j.value("ts", std::int64_t{0});
          r.journal = name;
          if (auto rr = insert_run(r); !rr) return fail_tx(rr.error());
        } catch (const json::exception&) {
          continue;
        }
        break;
      }
    }
  }

  // Meta: project, idea, view order.
  {
    json order = json::object();
    std::vector<std::string> top;
    for (const auto* b : schema->ordered_children("")) top.push_back(b->id);
    order[""] = top;
    for (const auto& b : schema->blocks) {
      if (b.children.empty()) continue;
      std::vector<std::string> kids;
      for (const auto* c : schema->ordered_children(b.id)) kids.push_back(c->id);
      order[b.id] = kids;
    }
    auto set = db_.q("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
    for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{
             {"project", schema->project.name},
             {"idea", schema->project.idea.value},
             {"order", order.dump()},
             {"fingerprint", fingerprint},
             {"updated_at", std::to_string(now)}}) {
      set.bind(1, k).bind(2, v);
      if (auto r = set.run(); !r) return fail_tx(r.error());
    }
  }
  if (auto r = db_.commit(); !r) return fail_tx(r.error());
  last_fingerprint_ = fingerprint;
  stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  spdlog::debug("index: {} blocks, {} files, {} analyzed, {:.1f} ms", stats.blocks, stats.files, stats.analyzed, stats.ms);
  return stats;
}

kk::VoidResult Index::insert_run(const RunRow& r) {
  auto s = db_.q(
      "INSERT OR IGNORE INTO runs(ticket, role, block, provider, model, tier, attempt, input_tokens, output_tokens, "
      "cache_write_tokens, cache_read_tokens, cost_usd, duration_ms, steps, outcome, ts, journal) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  s.bind_opt(1, r.ticket).bind(2, r.role).bind_opt(3, r.block).bind(4, r.provider).bind(5, r.model).bind(6, r.tier)
      .bind(7, r.attempt).bind(8, r.input_tokens).bind(9, r.output_tokens).bind(10, r.cache_write_tokens)
      .bind(11, r.cache_read_tokens).bind(12, r.cost_usd).bind(13, r.duration_ms).bind(14, r.steps)
      .bind(15, r.outcome).bind(16, r.ts).bind_opt(17, r.journal);
  return s.run();
}

std::vector<RunRow> Index::runs(std::string_view since_ts) {
  std::vector<RunRow> out;
  auto s = db_.q(
      "SELECT id, ticket, role, block, provider, model, tier, attempt, input_tokens, output_tokens, "
      "cache_write_tokens, cache_read_tokens, cost_usd, duration_ms, steps, outcome, ts, journal FROM runs "
      "WHERE ts >= ? ORDER BY ts, id");
  s.bind(1, static_cast<std::int64_t>(since_ts.empty() ? 0LL : std::stoll(std::string(since_ts))));
  while (s.step()) {
    RunRow r;
    r.id = s.i64(0);
    r.ticket = s.text(1);
    r.role = s.text(2);
    r.block = s.text(3);
    r.provider = s.text(4);
    r.model = s.text(5);
    r.tier = s.text(6);
    r.attempt = static_cast<int>(s.i64(7));
    r.input_tokens = s.i64(8);
    r.output_tokens = s.i64(9);
    r.cache_write_tokens = s.i64(10);
    r.cache_read_tokens = s.i64(11);
    r.cost_usd = s.f64(12);
    r.duration_ms = s.i64(13);
    r.steps = static_cast<int>(s.i64(14));
    r.outcome = s.text(15);
    r.ts = s.i64(16);
    r.journal = s.text(17);
    out.push_back(std::move(r));
  }
  return out;
}

Snapshot Index::snapshot() {
  Snapshot snap;
  {
    auto s = db_.q("SELECT key, value FROM meta WHERE key IN ('project', 'idea', 'order')");
    json order;
    while (s.step()) {
      auto k = s.text(0);
      if (k == "project") snap.project = s.text(1);
      if (k == "idea") snap.idea = s.text(1);
      if (k == "order") {
        try {
          order = json::parse(s.text(1));
        } catch (const json::exception&) {
        }
      }
    }
    auto b = db_.q("SELECT id, parent, kind, purpose, thread, est, depth, dir, card_hash, api_hash, api_frozen_hash, "
                   "status FROM blocks ORDER BY id");
    while (b.step()) {
      BlockRow r;
      r.id = b.text(0);
      r.parent = b.text(1);
      r.kind = b.text(2);
      r.purpose = b.text(3);
      r.thread = b.text(4);
      r.est = b.text(5);
      r.depth = static_cast<int>(b.i64(6));
      r.dir = b.text(7);
      r.card_hash = b.text(8);
      r.api_hash = b.text(9);
      r.api_frozen_hash = b.text(10);
      r.status = b.text(11);
      if (order.is_object() && order.contains(r.id)) r.children = order[r.id].get<std::vector<std::string>>();
      snap.blocks.push_back(std::move(r));
    }
    if (order.is_object() && order.contains("")) snap.top_level = order[""].get<std::vector<std::string>>();
  }
  {
    auto s = db_.q("SELECT block, name, dir, type, signature, line FROM ports ORDER BY block, line");
    while (s.step()) {
      snap.ports.push_back(PortRow{s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), static_cast<int>(s.i64(5))});
    }
  }
  {
    auto s = db_.q("SELECT id, src_block, src_port, dst_block, dst_port, kind, via, sync, rate, thread_src, "
                   "thread_dst, declared_in, line FROM links ORDER BY id");
    while (s.step()) {
      LinkRow l;
      l.id = s.i64(0);
      l.src_block = s.text(1);
      l.src_port = s.text(2);
      l.dst_block = s.text(3);
      l.dst_port = s.text(4);
      l.kind = s.text(5);
      l.via = s.text(6);
      l.sync = s.text(7);
      l.rate = s.text(8);
      l.thread_src = s.text(9);
      l.thread_dst = s.text(10);
      l.declared_in = s.text(11);
      l.line = static_cast<int>(s.i64(12));
      snap.links.push_back(std::move(l));
    }
  }
  {
    auto s = db_.q("SELECT block, text FROM invariants ORDER BY block, idx");
    while (s.step()) snap.invariants[s.text(0)].push_back(s.text(1));
    auto h = db_.q("SELECT block, ticket, summary FROM history ORDER BY block, ts");
    while (h.step()) snap.history[h.text(0)].emplace_back(h.text(1), h.text(2));
  }
  {
    auto s = db_.q("SELECT block, code, severity, message, file, line FROM drift ORDER BY file, line, code");
    while (s.step()) {
      snap.drift.push_back(DriftRow{s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), static_cast<int>(s.i64(5))});
    }
  }
  {
    auto s = db_.q("SELECT id, kind, status, origin, current_block, title, created_at, closed_at FROM tickets ORDER BY id");
    while (s.step()) {
      snap.tickets.push_back(TicketRow{s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), s.i64(6), s.i64(7)});
    }
  }
  {
    // Latest run per (link, metric).
    auto s = db_.q("SELECT link_id, link, metric, run, n, min, max, avg, last, ts FROM probes ORDER BY link, metric, ts, run");
    std::map<std::pair<std::string, std::string>, ProbeRow> latest;
    while (s.step()) {
      ProbeRow p;
      p.link_id = s.i64(0);
      p.link = s.text(1);
      p.metric = s.text(2);
      p.run = s.text(3);
      p.n = s.i64(4);
      p.min = s.f64(5);
      p.max = s.f64(6);
      p.avg = s.f64(7);
      p.last = s.f64(8);
      p.ts = s.i64(9);
      latest[{p.link, p.metric}] = p;
    }
    for (auto& [_, p] : latest) snap.probes.push_back(std::move(p));
  }
  {
    auto s = db_.q("SELECT block, contract_pass, contract_total, unit_pass, unit_total, integration_pass, "
                   "integration_total, build_failed, fresh, failures, ts FROM test_results ORDER BY block");
    while (s.step()) {
      TestRow t;
      t.block = s.text(0);
      t.contract = {static_cast<int>(s.i64(1)), static_cast<int>(s.i64(2))};
      t.unit = {static_cast<int>(s.i64(3)), static_cast<int>(s.i64(4))};
      t.integration = {static_cast<int>(s.i64(5)), static_cast<int>(s.i64(6))};
      t.build_failed = s.i64(7) != 0;
      t.fresh = s.i64(8) != 0;
      t.failures = core::split(s.text(9), '\n');
      t.ts = s.i64(10);
      snap.tests.push_back(std::move(t));
    }
  }
  // Own counters, then roll up into groups.
  std::map<std::string, int> impl_lines;
  {
    auto s = db_.q("SELECT block, SUM(lines) FROM files WHERE role='impl' GROUP BY block");
    while (s.step()) impl_lines[s.text(0)] = static_cast<int>(s.i64(1));
  }
  for (auto& b : snap.blocks) {
    if (const auto* t = snap.test(b.id); t != nullptr && t->fresh) {
      b.failed_tests = (t->contract.total - t->contract.pass) + (t->unit.total - t->unit.pass) +
                       (t->integration.total - t->integration.pass);
    }
    for (const auto& d : snap.drift) {
      if (d.block == b.id && d.severity == "E") ++b.drift_errors;
    }
    b.impl_lines = impl_lines[b.id];
  }
  for (auto& g : snap.blocks) {
    if (g.kind != "group") continue;
    int worst = -1;
    for (const auto& d : snap.blocks) {
      if (d.id == g.id || !Snapshot::under(g.id, d.id)) continue;
      if (d.kind != "group") {
        g.failed_tests += d.failed_tests;
        g.drift_errors += d.drift_errors;
        auto st = core::parse_block_status(d.status).value_or(core::BlockStatus::Design);
        worst = std::max(worst, core::badness(st));
      }
    }
    auto own = core::parse_block_status(g.status).value_or(core::BlockStatus::Design);
    if (own == core::BlockStatus::Drift) worst = std::max(worst, core::badness(own));
    if (worst >= 0) {
      for (auto st : {core::BlockStatus::Passing, core::BlockStatus::Implemented, core::BlockStatus::TestsReady,
                      core::BlockStatus::ApiFrozen, core::BlockStatus::Design, core::BlockStatus::NeedsReview,
                      core::BlockStatus::Drift, core::BlockStatus::Failing}) {
        if (core::badness(st) == worst) g.status = std::string(core::to_string(st));
      }
    }
    // Integration tests of the group itself count for the group.
    if (const auto* t = snap.test(g.id); t != nullptr && t->fresh) {
      int f = t->integration.total - t->integration.pass;
      g.failed_tests += f;
      if (f > 0 || t->build_failed) g.status = "failing";
    }
  }
  return snap;
}

}  // namespace kk::index
