#include "kk/agents/loop.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>

#include "kk/core/cancel.hpp"
#include "kk/core/text.hpp"
#include "kk/llm/provider.hpp"

namespace kk::agents {

using json = nlohmann::json;

const core::ModelTier* tier_config(const core::Config& cfg, const std::string& tier) { return cfg.tier(tier); }

AgentRunner::AgentRunner(fs::path project_root, const core::Config& cfg, ProviderFactory providers, index::Index* idx)
    : root_(std::move(project_root)), cfg_(cfg), providers_(std::move(providers)), idx_(idx) {}

namespace {

metrics::Tokens tokens_of(const llm::Usage& u) {
  return metrics::Tokens{u.input_tokens, u.output_tokens, u.cache_write_tokens - u.cache_write_1h_tokens,
                         u.cache_write_1h_tokens, u.cache_read_tokens};
}

// Journal payloads carry model output and file contents: invalid UTF-8 becomes U+FFFD
// instead of an exception that would abort the run.
constexpr auto kUtf8Replace = json::error_handler_t::replace;

std::string clip(const std::string& s, std::size_t n) { return core::truncate_utf8(s, n) + (s.size() <= n ? "" : "..."); }

}  // namespace

AgentResult AgentRunner::run(const AgentSpec& spec, IToolHost& host) {
  AgentResult res;
  auto t0 = std::chrono::steady_clock::now();
  const auto* tier = tier_config(cfg_, spec.tier);
  if (tier == nullptr) {
    res.outcome = "error";
    res.error = fmt::format("no [models.{}] in the config", spec.tier);
    return res;
  }
  res.model = tier->model;
  // Mode "mcp" (ADR 0024): an MCP client answers the turns — no API, no price.
  res.provider = cfg_.mcp() ? "mcp" : tier->provider;
  if (auto p = metrics::check_price(cfg_, tier->provider, tier->model); !p && !cfg_.mcp()) {
    res.outcome = "error";
    res.error = p.error().message;
    return res;
  }
  auto provider = providers_(spec.tier);
  if (!provider) {
    res.outcome = "error";
    res.error = provider.error().message;
    return res;
  }
  auto journal = metrics::RunJournal::open(root_, {spec.ticket, spec.role_name.empty() ? std::string(to_string(spec.role)) : spec.role_name,
                                                   spec.block.empty() ? "root" : spec.block, res.provider, tier->model,
                                                   spec.tier, spec.attempt});
  if (!journal) {
    res.outcome = "error";
    res.error = journal.error().message;
    return res;
  }
  res.journal = journal->path().filename().string();
  // Stop requests (ADR 0019): the process flag (SIGINT/SIGTERM) or the stop file of this
  // run (GUI). The file is looked at no more than every 200 ms; the HTTP transport and
  // the retry back-off ask the same scope while they wait.
  bool stop_seen = false;
  auto stop_checked = std::chrono::steady_clock::time_point{};
  core::CancelScope stop_scope([&] {
    if (stop_seen) return true;
    auto now = std::chrono::steady_clock::now();
    if (now - stop_checked < std::chrono::milliseconds(200)) return false;
    stop_checked = now;
    stop_seen = journal->stop_requested();
    return stop_seen;
  });

  // Prefix order for the cache (9.6): tools → system (marker) → stable context
  // (marker) → task; the last marker moves forward with every step.
  llm::Request req;
  req.model = tier->model;
  req.max_tokens = cfg_.max_output_tokens;
  if ((spec.role == Role::Orchestrator || spec.role == Role::Navigator) && cfg_.max_output_tokens_routing > 0) {
    req.max_tokens = req.max_tokens > 0 ? std::min(req.max_tokens, cfg_.max_output_tokens_routing) : cfg_.max_output_tokens_routing;
  }
  req.cache_ttl = cfg_.cache_ttl;
  // [effort] per role beats the tier's effort: a dispatch decision rarely needs deep reasoning.
  const std::string role_name = spec.role_name.empty() ? std::string(to_string(spec.role)) : spec.role_name;
  if (auto e = cfg_.role_effort.find(role_name); e != cfg_.role_effort.end()) {
    req.effort = e->second;
  } else {
    req.effort = tier->effort;
  }
  if (tier->provider == "anthropic" &&
      (tier->fallbacks == "default" || (tier->fallbacks == "auto" && llm::supports_default_fallback(tier->model)))) {
    req.fallbacks = "default";
  }
  for (const auto& t : host.tools()) req.tools.push_back({t.name, t.description, t.schema_json, false});
  req.system = {llm::ContentBlock::make_text(spec.system_text, true)};
  llm::Message first{"user", {}};
  if (!spec.context_text.empty()) first.content.push_back(llm::ContentBlock::make_text(spec.context_text, true));
  first.content.push_back(llm::ContentBlock::make_text(spec.task_text.empty() ? "Begin." : spec.task_text));
  req.messages.push_back(std::move(first));
  // Which run the turns belong to (the MCP bridge shows it to the client; APIs ignore it).
  req.meta_json = json{{"journal", res.journal},
                       {"ticket", spec.ticket},
                       {"role", role_name},
                       {"block", spec.block.empty() ? "root" : spec.block},
                       {"tier", spec.tier},
                       {"subagent", core::subagent_name(spec.tier)},
                       {"attempt", spec.attempt}}
                      .dump(-1, ' ', false, kUtf8Replace);
  (void)journal->step("start", json{{"role", to_string(spec.role)},
                                    {"block", spec.block},
                                    {"tier", spec.tier},
                                    {"system_tokens_est", core::estimate_tokens(spec.system_text)},
                                    {"context_tokens_est", core::estimate_tokens(spec.context_text)},
                                    {"effort", req.effort},
                                    {"task", clip(spec.task_text, 4000)},
                                    // What the model saw, so a run can be read back step by step.
                                    {"system", clip(spec.system_text, 65536)},
                                    {"context", clip(spec.context_text, 65536)}}
                                   .dump(-1, ' ', false, kUtf8Replace));

  bool nudged = false;  // the one "answer with a tool, not prose" reminder
  int cuts = 0;         // answers cut at the output limit, each followed by a "decide now" (at most 3)
  auto stop_now = [&] {
    if (!stop_seen) stop_seen = journal->stop_requested();  // unthrottled between steps
    return stop_seen || core::cancelled();
  };
  while (true) {
    if (stop_now()) {
      res.outcome = "cancelled";
      (void)journal->step("cancelled", json{{"message", "stopped by the user"}}.dump(-1, ' ', false, kUtf8Replace));
      break;
    }
    if (res.steps >= spec.max_steps) {
      res.outcome = "max_steps";
      break;
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    if (elapsed > spec.timeout) {
      res.outcome = "timeout";
      break;
    }
    auto resp = (*provider)->send(req);
    ++res.steps;
    if (!resp && (resp.error().code == "cancelled" || stop_now())) {
      res.outcome = "cancelled";
      (void)journal->step("cancelled", json{{"message", "stopped by the user"}}.dump(-1, ' ', false, kUtf8Replace));
      break;
    }
    if (!resp) {
      res.outcome = "error";
      res.error = resp.error().message;
      (void)journal->step("error", json{{"message", res.error}}.dump(-1, ' ', false, kUtf8Replace));
      break;
    }
    res.tokens += tokens_of(resp->usage);
    json content = json::array();
    for (const auto& b : resp->content) {
      if (!b.raw_json.empty()) {
        try {
          content.push_back(json::parse(b.raw_json));
          continue;
        } catch (const json::exception&) {
        }
      }
      if (b.type == llm::ContentBlock::Type::Text) content.push_back({{"type", "text"}, {"text", b.text}});
      if (b.type == llm::ContentBlock::Type::ToolUse) {
        content.push_back({{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", json::parse(b.input_json, nullptr, false)}});
      }
    }
    (void)journal->step("response", json{{"stop_reason", resp->stop_reason},
                                         {"usage",
                                          {{"input", resp->usage.input_tokens},
                                           {"output", resp->usage.output_tokens},
                                           {"reasoning", resp->usage.reasoning_tokens},
                                           {"cache_write", resp->usage.cache_write_tokens},
                                           {"cache_read", resp->usage.cache_read_tokens}}},
                                         {"content", content}}
                                        .dump(-1, ' ', false, kUtf8Replace));
    auto text = resp->text();
    if (!text.empty()) res.final_text = text;
    req.messages.push_back({"assistant", resp->content});
    // The budget follows what tokens cost: a cache read is 10-30x cheaper than input and every
    // step re-reads the whole prefix, so counting it fully stopped cheap 20-step runs midway
    // (a DeepSeek worker: 16k in, 14k out, 283k cache reads, $0.02, ADR 0021).
    long long used = res.tokens.input + res.tokens.output + res.tokens.cache_write() + res.tokens.cache_read / 10;
    if (resp->stop_reason == "refusal") {
      res.outcome = "refusal";
      break;
    }
    auto uses = resp->tool_uses();
    if (uses.empty()) {
      // Cut at the output limit while thinking: the API drops the hidden reasoning, so the
      // model would start over. Its last thoughts come back as text with a "decide now"
      // (seen with DeepSeek: 8192 reasoning tokens, then a fresh 8192).
      if (resp->stop_reason == "max_tokens" && cuts < 3 && res.steps < spec.max_steps) {
        ++cuts;
        std::string thought;
        for (const auto& b : resp->content) {
          if (b.type == llm::ContentBlock::Type::Other && !b.text.empty()) thought = b.text;
        }
        std::string tail = thought.size() > 2000 ? "..." + core::truncate_utf8(thought.substr(thought.size() - 2000), 2000)
                                                 : thought;
        std::string msg = "Your answer was cut at the output limit before any tool call.";
        if (!tail.empty()) msg += " Your last thoughts were:\n<<<\n" + tail + "\n>>>\nContinue from there:";
        msg += " think briefly and decide now with one tool call; if you are unsure, take the most likely option.";
        req.messages.push_back({"user", {llm::ContentBlock::make_text(msg, true)}});
        continue;
      }
      if (!nudged && res.steps < spec.max_steps) {
        nudged = true;
        req.messages.push_back({"user", {llm::ContentBlock::make_text(
                                            "Finish by calling one of your tools (for example report). Do not answer in prose.", true)}});
        continue;
      }
      res.outcome = "end_turn";
      break;
    }
    llm::Message results{"user", {}};
    bool terminal = false;
    for (const auto* u : uses) {
      ToolOutcome out;
      try {
        out = host.call({u->id, u->name, u->input_json});
      } catch (const std::exception& e) {
        out = {std::string("tool failed: ") + e.what(), true, false};
      }
      (void)journal->step("tool", json{{"name", u->name},
                                       {"input", json::parse(u->input_json, nullptr, false)},
                                       {"is_error", out.is_error},
                                       {"terminal", out.terminal},
                                       {"result", clip(out.content, 2000)}}
                                      .dump(-1, ' ', false, kUtf8Replace));
      if (out.terminal && !terminal) {
        terminal = true;
        res.terminal_tool = u->name;
        res.terminal_input = u->input_json;
      }
      results.content.push_back(llm::ContentBlock::make_tool_result(u->id, out.content, out.is_error));
    }
    if (terminal) {
      res.outcome = "terminal";
      break;
    }
    if (spec.max_tokens > 0 && used > spec.max_tokens) {
      res.outcome = "budget";
      break;
    }
    // Move the cache marker to the newest stable block: the latest tool results.
    if (!results.content.empty()) results.content.back().cache = true;
    req.messages.push_back(std::move(results));
  }
  res.reads = host.reads();
  res.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  auto cost = metrics::cost_usd(cfg_, tier->provider, tier->model, res.tokens);
  res.cost = cost ? *cost : 0.0;
  std::string outcome = res.outcome == "terminal" ? res.terminal_tool : res.outcome;
  if (res.terminal_tool == "report") {
    auto j = json::parse(res.terminal_input, nullptr, false);
    if (j.is_object()) outcome = j.value("status", outcome);
  }
  (void)journal->step("end", json{{"outcome", res.outcome}, {"terminal", res.terminal_tool}, {"reads", res.reads}}.dump(-1, ' ', false, kUtf8Replace));
  auto row = journal->finish(res.tokens, res.cost, res.ms, res.steps, outcome, idx_);
  if (!row) spdlog::warn("agents: cannot finish run journal: {}", row.error().message);
  spdlog::info("agents: {} {} tier={} steps={} outcome={} in={} out={} cache_read={} usd={:.4f}", role_name,
               spec.block, spec.tier, res.steps, outcome, res.tokens.input, res.tokens.output, res.tokens.cache_read,
               res.cost);
  return res;
}

}  // namespace kk::agents
