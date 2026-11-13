#include "kk/metrics/metrics.hpp"

#include <fmt/format.h>

#include <unistd.h>

#include <csignal>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::metrics {

using json = nlohmann::json;

Tokens& Tokens::operator+=(const Tokens& o) {
  input += o.input;
  output += o.output;
  cache_write_5m += o.cache_write_5m;
  cache_write_1h += o.cache_write_1h;
  cache_read += o.cache_read;
  return *this;
}

bool is_local_provider(std::string_view provider) { return provider == "openai_compat"; }

kk::VoidResult check_price(const core::Config& cfg, std::string_view provider, std::string_view model) {
  if (cfg.prices.contains(std::string(model)) || is_local_provider(provider)) return {};
  return kk::fail("price",
                  fmt::format("no price for model '{}' in [prices] of karkas.toml; fill it from the official pricing page",
                              model));
}

kk::Result<double> cost_usd(const core::Config& cfg, std::string_view provider, std::string_view model,
                            const Tokens& t) {
  auto it = cfg.prices.find(std::string(model));
  if (it == cfg.prices.end()) {
    if (is_local_provider(provider)) return 0.0;
    auto r = check_price(cfg, provider, model);
    return std::unexpected(r.error());
  }
  const auto& p = it->second;
  double in = p.input;
  double w5 = p.cache_write_5m >= 0 ? p.cache_write_5m : in * cfg.cache_write_5m;
  double w1 = p.cache_write_1h >= 0 ? p.cache_write_1h : in * cfg.cache_write_1h;
  double rd = p.cache_read >= 0 ? p.cache_read : in * cfg.cache_read;
  double usd = static_cast<double>(t.input) * in + static_cast<double>(t.cache_write_5m) * w5 +
               static_cast<double>(t.cache_write_1h) * w1 + static_cast<double>(t.cache_read) * rd +
               static_cast<double>(t.output) * p.output;
  return usd / 1'000'000.0;
}

namespace {

std::string stamp() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
  return fmt::format("{}-{:03d}", buf, ms);
}

std::string safe(std::string s) {
  for (auto& c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) c = '_';
  }
  return s.empty() ? "-" : s;
}

}  // namespace

kk::Result<RunJournal> RunJournal::open(const fs::path& project_root, RunInfo info) {
  RunJournal j;
  auto dir = project_root / ".karkas" / "runs";
  std::error_code ec;
  fs::create_directories(dir / "active", ec);
  auto base = fmt::format("{}-{}-{}", stamp(), safe(info.role), safe(info.block));
  j.path_ = dir / (base + ".jsonl");
  for (int i = 2; fs::exists(j.path_); ++i) j.path_ = dir / fmt::format("{}-{}.jsonl", base, i);
  j.marker_ = dir / "active" / (j.path_.stem().string() + ".json");
  j.stop_ = dir / "active" / (j.path_.stem().string() + ".stop");
  j.info_ = std::move(info);
  json start = {{"type", "run_start"}, {"ticket", j.info_.ticket}, {"role", j.info_.role},  {"block", j.info_.block},
                {"provider", j.info_.provider}, {"model", j.info_.model}, {"tier", j.info_.tier},
                {"attempt", j.info_.attempt}, {"ts", core::unix_now()}, {"pid", ::getpid()}};
  if (auto r = core::append_file(j.path_, start.dump() + "\n"); !r) return std::unexpected(r.error());
  json marker = start;
  marker["journal"] = j.path_.filename().string();
  if (auto r = core::write_file(j.marker_, marker.dump()); !r) return std::unexpected(r.error());
  return j;
}

RunJournal::~RunJournal() {
  if (!finished_ && !marker_.empty()) {
    std::error_code ec;
    fs::remove(marker_, ec);
    fs::remove(stop_, ec);
  }
}

bool RunJournal::stop_requested() const {
  std::error_code ec;
  return !stop_.empty() && fs::exists(stop_, ec);
}

kk::VoidResult RunJournal::step(std::string_view kind, std::string_view payload_json) {
  json payload;
  try {
    payload = json::parse(payload_json.empty() ? "null" : payload_json);
  } catch (const json::exception&) {
    payload = std::string(payload_json);
  }
  json line = {{"type", "step"}, {"n", ++n_}, {"kind", kind}, {"ts", core::unix_now()}, {"data", payload}};
  return core::append_file(path_, line.dump(-1, ' ', false, json::error_handler_t::replace) + "\n");
}

kk::Result<index::RunRow> RunJournal::finish(const Tokens& t, double cost, std::int64_t duration_ms, int steps,
                                             std::string_view outcome, index::Index* idx) {
  index::RunRow r;
  r.ticket = info_.ticket;
  r.role = info_.role;
  r.block = info_.block;
  r.provider = info_.provider;
  r.model = info_.model;
  r.tier = info_.tier;
  r.attempt = info_.attempt;
  r.input_tokens = t.input;
  r.output_tokens = t.output;
  r.cache_write_tokens = t.cache_write();
  r.cache_read_tokens = t.cache_read;
  r.cost_usd = cost;
  r.duration_ms = duration_ms;
  r.steps = steps;
  r.outcome = std::string(outcome);
  r.ts = core::unix_now();
  r.journal = path_.filename().string();
  json end = {{"type", "run_end"},          {"ticket", r.ticket},
              {"role", r.role},             {"block", r.block},
              {"provider", r.provider},     {"model", r.model},
              {"tier", r.tier},             {"attempt", r.attempt},
              {"input_tokens", r.input_tokens}, {"output_tokens", r.output_tokens},
              {"cache_write_tokens", r.cache_write_tokens}, {"cache_write_1h_tokens", t.cache_write_1h},
              {"cache_read_tokens", r.cache_read_tokens}, {"cost_usd", r.cost_usd},
              {"duration_ms", r.duration_ms}, {"steps", r.steps},
              {"outcome", r.outcome},       {"ts", r.ts}};
  if (auto w = core::append_file(path_, end.dump() + "\n"); !w) return std::unexpected(w.error());
  finished_ = true;
  std::error_code ec;
  fs::remove(marker_, ec);
  fs::remove(stop_, ec);
  if (idx != nullptr) {
    if (auto ins = idx->insert_run(r); !ins) return std::unexpected(ins.error());
  }
  return r;
}

std::vector<ActiveRun> active_runs(const fs::path& project_root) {
  std::vector<ActiveRun> out;
  std::error_code ec;
  auto dir = project_root / ".karkas" / "runs" / "active";
  std::vector<fs::path> stops;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() == ".stop") stops.push_back(e.path());
    if (e.path().extension() != ".json") continue;
    auto text = core::read_file(e.path());
    if (!text) continue;
    try {
      auto j = json::parse(*text);
      // A marker whose owner process is gone is stale: the run was killed and its
      // destructor never removed the marker. Drop it so the GUI does not show a
      // phantom "agent working".
      int pid = j.value("pid", 0);
      if (pid > 0 && ::kill(pid, 0) != 0 && errno == ESRCH) {
        fs::remove(e.path(), ec);
        continue;
      }
      ActiveRun a{j.value("role", ""), j.value("block", ""), j.value("model", ""), j.value("ticket", ""),
                  j.value("ts", std::int64_t{0}), e.path().parent_path().parent_path() / j.value("journal", "")};
      a.pid = pid;
      auto stop = e.path();
      stop.replace_extension(".stop");
      a.stopping = fs::exists(stop, ec);
      out.push_back(std::move(a));
    } catch (const json::exception&) {
    }
  }
  // A stop request whose run has ended (or was killed) means nothing any more.
  for (const auto& s : stops) {
    auto marker = s;
    marker.replace_extension(".json");
    if (!fs::exists(marker, ec)) fs::remove(s, ec);
  }
  std::sort(out.begin(), out.end(), [](const ActiveRun& a, const ActiveRun& b) {
    return a.started != b.started ? a.started < b.started : a.journal < b.journal;
  });
  return out;
}

kk::VoidResult request_stop(const fs::path& project_root, const fs::path& journal) {
  auto dir = project_root / ".karkas" / "runs" / "active";
  auto stem = journal.stem().string();
  std::error_code ec;
  if (stem.empty() || !fs::exists(dir / (stem + ".json"), ec)) return kk::fail("stop", "the run is not active: " + journal.filename().string());
  return core::write_file(dir / (stem + ".stop"), fmt::format("{}\n", core::unix_now()));
}

kk::Result<std::int64_t> parse_since(std::string_view s) {
  s = core::trim(s);
  if (s.empty()) return 0;
  if (s.size() == 10 && s[4] == '-' && s[7] == '-') {
    std::tm tm{};
    tm.tm_year = std::stoi(std::string(s.substr(0, 4))) - 1900;
    tm.tm_mon = std::stoi(std::string(s.substr(5, 2))) - 1;
    tm.tm_mday = std::stoi(std::string(s.substr(8, 2)));
    return static_cast<std::int64_t>(timegm(&tm));
  }
  try {
    return std::stoll(std::string(s));
  } catch (const std::exception&) {
    return kk::fail("args", fmt::format("--since expects YYYY-MM-DD or a unix time, got '{}'", s));
  }
}

kk::Result<std::string> cost_report(const core::Config& cfg, const std::vector<index::RunRow>& runs,
                                    std::string_view by) {
  std::set<std::string> missing;
  for (const auto& r : runs) {
    if (!check_price(cfg, r.provider, r.model)) missing.insert(r.model);
  }
  if (!missing.empty()) {
    return kk::fail("price", fmt::format("no price in [prices] for: {} (karkas cost never counts a cloud model as free)",
                                         core::join({missing.begin(), missing.end()}, ", ")));
  }
  struct Agg {
    int runs = 0;
    std::int64_t in = 0, out = 0, cw = 0, cr = 0, ms = 0;
    double usd = 0;
    void add(const index::RunRow& r, double cost) {
      ++runs;
      in += r.input_tokens;
      out += r.output_tokens;
      cw += r.cache_write_tokens;
      cr += r.cache_read_tokens;
      ms += r.duration_ms;
      usd += cost;
    }
  };
  std::map<std::string, Agg> groups;
  Agg total;
  for (const auto& r : runs) {
    std::string key;
    if (by == "block") key = r.block;
    else if (by == "role") key = r.role;
    else if (by == "model") key = r.model;
    else if (by == "ticket") key = r.ticket;
    else if (by.empty() || by == "total") key = "";
    else return kk::fail("args", fmt::format("--by expects block|role|model|ticket, got '{}'", by));
    if (key.empty() && !by.empty() && by != "total") key = "-";
    // Money from the current [prices], not the value stored when the run finished:
    // prices can be filled in later (16.2), and a report must reflect them.
    double cost = 0;
    auto c = cost_usd(cfg, r.provider, r.model, Tokens{r.input_tokens, r.output_tokens, r.cache_write_tokens, 0, r.cache_read_tokens});
    if (c) cost = *c;
    if (!key.empty()) groups[key].add(r, cost);
    total.add(r, cost);
  }
  auto share = [](const Agg& a) {
    double denom = static_cast<double>(a.in + a.cw + a.cr);
    return denom > 0 ? 100.0 * static_cast<double>(a.cr) / denom : 0.0;
  };
  auto row = [&](const std::string& k, const Agg& a) {
    return fmt::format("{:<24} {:>5} {:>10} {:>9} {:>11} {:>10} {:>6.1f}% {:>10.4f}\n", k, a.runs, a.in, a.out, a.cw,
                       a.cr, share(a), a.usd);
  };
  std::string out = fmt::format("{:<24} {:>5} {:>10} {:>9} {:>11} {:>10} {:>7} {:>10}\n", by.empty() ? "" : by, "runs",
                                "input", "output", "cache_write", "cache_read", "cached", "usd");
  for (const auto& [k, a] : groups) out += row(k, a);
  out += row("total", total);
  return out;
}

}  // namespace kk::metrics

namespace kk::metrics {

namespace {

bool ended_badly(const std::string& outcome) {
  static const std::set<std::string> bad = {"failed", "max_steps", "budget", "timeout", "error", "cancelled", "escalated"};
  return bad.contains(outcome);
}

struct Traffic {
  int turns = 0, whole = 0, seen = 0;
  std::int64_t shown = 0, whole_chars = 0, answers = 0;
};

Traffic read_traffic(const std::filesystem::path& file) {
  Traffic t;
  auto text = core::read_file(file);
  if (!text) return t;
  for (const auto& line : core::split(*text, '\n')) {
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (!j.is_object()) continue;
    if (j.contains("shown")) {
      ++t.turns;
      auto n = j.value("shown", std::int64_t{0});
      t.shown += n;
      if (j.value("whole", false)) {
        ++t.whole;
        t.whole_chars += n;
      }
      if (j.value("seen", false)) ++t.seen;
    }
    if (j.contains("answer")) t.answers += j.value("answer", std::int64_t{0});
  }
  return t;
}

}  // namespace

std::string stats_report(const std::vector<index::RunRow>& runs, const std::string& ticket, const std::filesystem::path& project_root) {
  std::string out;
  if (ticket.empty()) {
    struct Acc {
      int runs = 0, ok = 0, steps = 0, escalated = 0;
      double usd = 0;
    };
    std::map<std::pair<std::string, std::string>, Acc> by;
    for (const auto& r : runs) {
      auto& a = by[{r.role, r.tier}];
      ++a.runs;
      a.ok += ended_badly(r.outcome) ? 0 : 1;
      a.steps += r.steps;
      a.usd += r.cost_usd;
      a.escalated += r.attempt > 1 ? 1 : 0;
    }
    out += fmt::format("{:<20} {:<16} {:>5} {:>9} {:>6} {:>9} {:>9}\n", "role", "tier", "runs", "ended ok", "steps", "usd", "retries");
    for (const auto& [k, a] : by) {
      out += fmt::format("{:<20} {:<16} {:>5} {:>8}% {:>6.1f} {:>9.4f} {:>9}\n", k.first, k.second, a.runs,
                         a.runs == 0 ? 0 : 100 * a.ok / a.runs, a.runs == 0 ? 0.0 : static_cast<double>(a.steps) / a.runs, a.usd,
                         a.escalated);
    }
    return out;
  }
  int n = 0, steps = 0;
  double usd = 0;
  Traffic total;
  std::map<std::string, int> by_role;
  for (const auto& r : runs) {
    if (r.ticket != ticket) continue;
    ++n;
    steps += r.steps;
    usd += r.cost_usd;
    ++by_role[r.role];
    auto stem = r.journal;
    if (auto slash = stem.find_last_of('/'); slash != std::string::npos) stem = stem.substr(slash + 1);
    if (stem.ends_with(".jsonl")) stem.resize(stem.size() - 6);
    auto t = read_traffic(project_root / ".karkas" / "mcp" / "traffic" / (stem + ".jsonl"));
    total.turns += t.turns;
    total.whole += t.whole;
    total.seen += t.seen;
    total.shown += t.shown;
    total.whole_chars += t.whole_chars;
    total.answers += t.answers;
    out += fmt::format("{:<14} {:<16} {:<14} steps={:<3} {:<22} usd={:.4f}{}\n", r.role, r.block.empty() ? "root" : r.block, r.tier, r.steps,
                       r.outcome, r.cost_usd,
                       t.turns > 0 ? fmt::format("  turns={} whole={} chars={}", t.turns, t.whole, t.shown) : std::string());
  }
  if (n == 0) return "no runs of " + ticket + "\n";
  std::vector<std::string> roles;
  for (const auto& [role, c] : by_role) roles.push_back(fmt::format("{} {}", c, role));
  out += fmt::format("total: {} runs ({}), {} steps, usd={:.4f}\n", n, core::join(roles, ", "), steps, usd);
  if (total.turns > 0) {
    out += fmt::format("mcp traffic: {} turns, {} of them whole ({} shorter by seen), {} chars shown ({}% in whole turns), {} chars "
                       "answered\n",
                       total.turns, total.whole, total.seen, total.shown, total.shown == 0 ? 0 : 100 * total.whole_chars / total.shown,
                       total.answers);
  }
  return out;
}

}  // namespace kk::metrics
