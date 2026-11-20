#include "kk/llm/mcp_bridge.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <thread>

#include "kk/core/cancel.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::llm {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr auto kReplace = json::error_handler_t::replace;

std::string str(const json& j, const char* k) {
  if (!j.is_object() || !j.contains(k) || !j[k].is_string()) return {};
  return j[k].get<std::string>();
}

std::int64_t now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Written whole or not at all: the reader never sees half a file.
kk::VoidResult write_atomic(const fs::path& path, const std::string& text) {
  auto tmp = path;
  tmp += ".tmp";
  if (auto r = core::write_file(tmp, text); !r) return r;
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) return kk::fail("io", fmt::format("cannot write {}: {}", path.string(), ec.message()));
  return {};
}

json block_json(const ContentBlock& b) {
  switch (b.type) {
    case ContentBlock::Type::Text:
      return {{"type", "text"}, {"text", b.text}};
    case ContentBlock::Type::ToolUse:
      return {{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", json::parse(b.input_json, nullptr, false)}};
    case ContentBlock::Type::ToolResult:
      return {{"type", "tool_result"}, {"tool_use_id", b.tool_use_id}, {"content", b.text}, {"is_error", b.is_error}};
    case ContentBlock::Type::Other:
      break;
  }
  return nullptr;
}

}  // namespace

// --- queue ---------------------------------------------------------------------------------------

McpQueue::McpQueue(fs::path dir) : dir_(std::move(dir)) {}

std::vector<McpTurn> McpQueue::waiting() const {
  std::vector<McpTurn> out;
  std::error_code ec;
  auto dir = turns_dir();
  if (!fs::is_directory(dir, ec)) return out;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    auto name = e.path().filename().string();
    if (!name.ends_with(".meta.json")) continue;
    std::string id = name.substr(0, name.size() - std::string(".meta.json").size());
    if (fs::exists(dir / (id + ".resp.json"), ec)) continue;
    if (auto t = turn(id)) out.push_back(std::move(*t));
  }
  std::sort(out.begin(), out.end(), [](const McpTurn& a, const McpTurn& b) { return a.ts != b.ts ? a.ts < b.ts : a.id < b.id; });
  return out;
}

std::optional<McpTurn> McpQueue::turn(const std::string& id) const {
  auto text = core::read_file(turns_dir() / (id + ".meta.json"));
  if (!text) return std::nullopt;
  auto j = json::parse(*text, nullptr, false);
  if (!j.is_object()) return std::nullopt;
  McpTurn t;
  t.id = id;
  t.run = str(j, "run");
  t.ticket = str(j, "ticket");
  t.role = str(j, "role");
  t.block = str(j, "block");
  t.tier = str(j, "tier");
  t.model = str(j, "model");
  t.subagent = str(j, "subagent");
  t.seq = j.value("seq", 0);
  t.ts = j.value("ts", static_cast<std::int64_t>(0));
  std::error_code ec;
  auto taken = turns_dir() / (id + ".taken");
  if (fs::exists(taken, ec)) {
    t.taken = true;
    if (auto tt = core::read_file(taken)) t.taken_ts = std::atoll(tt->c_str());
  }
  return t;
}

kk::Result<std::string> McpQueue::request(const std::string& id) const { return core::read_file(turns_dir() / (id + ".req.json")); }

void McpQueue::take(const std::string& id) const { (void)write_atomic(turns_dir() / (id + ".taken"), std::to_string(now_s())); }

void McpQueue::release(const std::string& id) const {
  std::error_code ec;
  fs::remove(turns_dir() / (id + ".taken"), ec);
}

void McpQueue::pause_until(std::int64_t ts) const {
  std::error_code ec;
  if (ts <= 0) {
    fs::remove(dir_ / "paused_until", ec);
    return;
  }
  fs::create_directories(dir_, ec);
  (void)write_atomic(dir_ / "paused_until", std::to_string(ts));
}

std::int64_t McpQueue::paused_until() const {
  auto t = core::read_file(dir_ / "paused_until");
  return t ? std::atoll(t->c_str()) : 0;
}

kk::VoidResult McpQueue::answer(const std::string& id, const std::string& answer_json) const {
  std::error_code ec;
  if (!fs::exists(turns_dir() / (id + ".req.json"), ec)) return kk::fail("mcp", "no turn " + id + " waits (answered already, or its run ended)");
  return write_atomic(turns_dir() / (id + ".resp.json"), answer_json);
}

kk::VoidResult McpQueue::put(const McpTurn& meta, const std::string& request_json) const {
  std::error_code ec;
  fs::create_directories(turns_dir(), ec);
  json m = {{"run", meta.run},     {"ticket", meta.ticket}, {"role", meta.role},         {"block", meta.block},
            {"tier", meta.tier},   {"model", meta.model},   {"subagent", meta.subagent}, {"seq", meta.seq},
            {"ts", meta.ts}};
  if (auto r = write_atomic(turns_dir() / (meta.id + ".req.json"), request_json); !r) return r;
  return write_atomic(turns_dir() / (meta.id + ".meta.json"), m.dump(-1, ' ', false, kReplace));
}

std::optional<std::string> McpQueue::response(const std::string& id) const {
  std::error_code ec;
  auto p = turns_dir() / (id + ".resp.json");
  if (!fs::exists(p, ec)) return std::nullopt;
  auto text = core::read_file(p);
  if (!text) return std::nullopt;
  return *text;
}

void McpQueue::remove(const std::string& id) const {
  std::error_code ec;
  for (const char* ext : {".meta.json", ".req.json", ".taken", ".resp.json"}) fs::remove(turns_dir() / (id + ext), ec);
}

// --- request / answer --------------------------------------------------------------------------

std::string turn_request_json(const Request& req, const std::string& id) {
  std::string system;
  for (const auto& b : req.system) {
    if (b.type != ContentBlock::Type::Text) continue;
    if (!system.empty()) system += "\n\n";
    system += b.text;
  }
  json tools = json::array();
  for (const auto& t : req.tools) {
    tools.push_back({{"name", t.name}, {"description", t.description}, {"input_schema", json::parse(t.input_schema_json, nullptr, false)}});
  }
  json messages = json::array();
  for (const auto& m : req.messages) {
    json content = json::array();
    for (const auto& b : m.content) {
      if (auto j = block_json(b); !j.is_null()) content.push_back(std::move(j));
    }
    messages.push_back({{"role", m.role}, {"content", std::move(content)}});
  }
  json j = {{"id", id}, {"model", req.model}, {"system", system}, {"tools", tools}, {"messages", messages}};
  return j.dump(-1, ' ', false, kReplace);
}

kk::Result<Response> turn_response(const std::string& answer_json, const std::string& id, const std::string& model) {
  auto j = json::parse(answer_json, nullptr, false);
  if (!j.is_object()) return kk::fail("mcp", "the answer of turn " + id + " is not a JSON object");
  Response r;
  r.id = id;
  r.model = model;
  if (auto t = str(j, "text"); !core::trim(t).empty()) r.content.push_back(ContentBlock::make_text(t));
  int n = 0;
  if (j.contains("calls") && j["calls"].is_array()) {
    for (const auto& c : j["calls"]) {
      if (!c.is_object() || str(c, "name").empty()) continue;
      ContentBlock b;
      b.type = ContentBlock::Type::ToolUse;
      b.name = str(c, "name");
      b.id = str(c, "id").empty() ? fmt::format("mcp_{}_{}", id, ++n) : str(c, "id");
      // An identifier the engine can send back: letters, digits, '_' and '-'.
      for (auto& ch : b.id) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-')) ch = '_';
      }
      json input = c.contains("input") ? c["input"] : json::object();
      if (input.is_string()) input = json::parse(input.get<std::string>(), nullptr, false);  // a client may send it as text
      if (!input.is_object()) input = json::object();
      b.input_json = input.dump(-1, ' ', false, kReplace);
      r.content.push_back(std::move(b));
    }
  }
  r.stop_reason = r.tool_uses().empty() ? "end_turn" : "tool_use";
  return r;
}

// --- provider ----------------------------------------------------------------------------------

McpBridgeProvider::McpBridgeProvider(fs::path queue_dir, int wait_s)
    : dir_(queue_dir), queue_(std::move(queue_dir)), wait_s_(std::max(1, wait_s)) {}

// The client's tokens of a turn, estimated from what the MCP server showed it and what it answered
// (.karkas/mcp/traffic/<run>.jsonl, ADR 0026) the way a Claude Code session spends them (docs/adr/0031):
// - everything new in its context is written to the prompt cache once, with a 1-hour TTL (2x the input
//   price): what this turn showed, the model's own output, and on the run's first turn the session's
//   base context (its system prompt and the two tools, ~2K tokens);
// - every request reads the context cached so far again (cache reads);
// - the output is about five times the text of the answer: hidden thinking and the tool call's JSON.
// The old estimate (shown and answered text at the full input price, no cache) was 3-5x below the
// sessions' own cost on Endless Sky (round 3: $0.19 against $1.08); this one is within 10-20% there
// (fitted on round 3 run 3, checked on round 2 runs 4 and 5 and round 3 run 1). The real cost of a
// session Karkas launched replaces it when the session ends (index: costs.jsonl).
Usage McpBridgeProvider::traffic_usage(const McpTurn& t) const {
  Usage u;
  auto stem = t.run.ends_with(".jsonl") ? t.run.substr(0, t.run.size() - 6) : t.run;
  auto text = core::read_file(dir_ / "traffic" / (stem + ".jsonl"));
  if (!text) return u;
  // The run's turns in order: characters shown and answered.
  std::vector<std::string> order;
  std::map<std::string, std::pair<std::int64_t, std::int64_t>> turns;
  for (const auto& line : core::split(*text, '\n')) {
    auto j = json::parse(line, nullptr, false);
    if (!j.is_object()) continue;
    auto id = j.value("turn", "");
    if (id.empty()) continue;
    if (!turns.contains(id)) order.push_back(id);
    auto& [shown, answer] = turns[id];
    if (j.contains("shown")) shown = j.value("shown", std::int64_t{0});
    if (j.contains("answer")) answer = j.value("answer", std::int64_t{0});
  }
  constexpr double kCharsPerToken = 3.5;
  constexpr double kBaseContext = 2000;  // the session's own system prompt and tools
  constexpr double kOutputPerAnswer = 5;  // thinking and the call's JSON per token of answer text
  double cached = 0;  // the context written to the cache by the turns before
  for (std::size_t i = 0; i < order.size(); ++i) {
    const auto& [shown, answer] = turns[order[i]];
    const double out = static_cast<double>(answer) / kCharsPerToken * kOutputPerAnswer;
    const double base = i == 0 ? kBaseContext : 0;
    const double write = static_cast<double>(shown) / kCharsPerToken + out + base;
    if (order[i] == t.id) {
      u.output_tokens = static_cast<std::int64_t>(out);
      u.cache_write_tokens = static_cast<std::int64_t>(write);
      u.cache_write_1h_tokens = u.cache_write_tokens;
      u.cache_read_tokens = static_cast<std::int64_t>(cached + base);
      u.input_tokens = 20;  // the few uncached tokens a request sends
      break;
    }
    cached += write;
  }
  return u;
}

kk::Result<Response> McpBridgeProvider::send(const Request& req) {
  ++seq_;
  auto meta = json::parse(req.meta_json.empty() ? "{}" : req.meta_json, nullptr, false);
  McpTurn t;
  t.run = str(meta, "journal");
  std::string stem = t.run.empty() ? fmt::format("run-{}", now_s()) : t.run;
  if (stem.ends_with(".jsonl")) stem.resize(stem.size() - 6);
  t.id = fmt::format("{}-{:03d}", stem, seq_);
  t.ticket = str(meta, "ticket");
  t.role = str(meta, "role");
  t.block = str(meta, "block");
  t.tier = str(meta, "tier");
  t.model = req.model;
  t.subagent = str(meta, "subagent");
  t.seq = seq_;
  t.ts = now_s();
  if (auto r = queue_.put(t, turn_request_json(req, t.id)); !r) return std::unexpected(r.error());
  spdlog::info("mcp: turn {} waits for the MCP client ({} {} {})", t.id, t.role, t.block, t.tier);
  auto t0 = std::chrono::steady_clock::now();
  int polls = 0;
  while (true) {
    // While no session can start (the usage limit), the wait does not count.
    if (++polls % 16 == 0 && queue_.paused_until() > now_s()) t0 = std::chrono::steady_clock::now();
    if (auto a = queue_.response(t.id)) {
      queue_.remove(t.id);
      auto r = turn_response(*a, t.id, req.model);
      if (r) r->usage = traffic_usage(t);
      return r;
    }
    if (core::cancelled()) {
      queue_.remove(t.id);
      return kk::fail("cancelled", "stopped by the user");
    }
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(wait_s_)) {
      queue_.remove(t.id);
      return kk::fail("mcp", fmt::format("no MCP client answered turn {} in {} s. Agents run in mode \"mcp\": connect Claude Code "
                                         "to `karkas mcp` and let it serve the turns (kk_agent_next / kk_agent_act), or switch "
                                         "[agents] mode to \"api\"",
                                         t.id, wait_s_));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
  }
}

kk::Result<std::int64_t> McpBridgeProvider::count_tokens(const Request& req) {
  std::int64_t n = 0;
  for (const auto& b : req.system) n += static_cast<std::int64_t>(core::estimate_tokens(b.text));
  for (const auto& m : req.messages) {
    for (const auto& b : m.content) n += static_cast<std::int64_t>(core::estimate_tokens(b.text) + core::estimate_tokens(b.input_json));
  }
  return n;
}

}  // namespace kk::llm
