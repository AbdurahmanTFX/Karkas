#include "kk/metrics/metrics.hpp"

#include <fmt/format.h>

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
  j.info_ = std::move(info);
  json start = {{"type", "run_start"}, {"ticket", j.info_.ticket}, {"role", j.info_.role},  {"block", j.info_.block},
                {"provider", j.info_.provider}, {"model", j.info_.model}, {"tier", j.info_.tier},
                {"attempt", j.info_.attempt}, {"ts", core::unix_now()}};
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
  }
}

kk::VoidResult RunJournal::step(std::string_view kind, std::string_view payload_json) {
  json payload;
  try {
    payload = json::parse(payload_json.empty() ? "null" : payload_json);
  } catch (const json::exception&) {
    payload = std::string(payload_json);
  }
  json line = {{"type", "step"}, {"n", ++n_}, {"kind", kind}, {"ts", core::unix_now()}, {"data", payload}};
  return core::append_file(path_, line.dump() + "\n");
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
  if (idx != nullptr) {
    if (auto ins = idx->insert_run(r); !ins) return std::unexpected(ins.error());
  }
  return r;
}

std::vector<ActiveRun> active_runs(const fs::path& project_root) {
  std::vector<ActiveRun> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(project_root / ".karkas" / "runs" / "active", ec)) {
    auto text = core::read_file(e.path());
    if (!text) continue;
    try {
      auto j = json::parse(*text);
      out.push_back(ActiveRun{j.value("role", ""), j.value("block", ""), j.value("model", ""), j.value("ticket", ""),
                              j.value("ts", std::int64_t{0}), e.path().parent_path().parent_path() / j.value("journal", "")});
    } catch (const json::exception&) {
    }
  }
  return out;
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
    void add(const index::RunRow& r) {
      ++runs;
      in += r.input_tokens;
      out += r.output_tokens;
      cw += r.cache_write_tokens;
      cr += r.cache_read_tokens;
      ms += r.duration_ms;
      usd += r.cost_usd;
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
    if (!key.empty()) groups[key].add(r);
    total.add(r);
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
