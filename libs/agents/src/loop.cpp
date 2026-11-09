#include "kk/agents/loop.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

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

std::string clip(const std::string& s, std::size_t n) { return s.size() <= n ? s : s.substr(0, n) + "..."; }

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
  res.provider = tier->provider;
  if (auto p = metrics::check_price(cfg_, tier->provider, tier->model); !p) {
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
                                                   spec.block.empty() ? "root" : spec.block, tier->provider, tier->model,
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
  req.cache_ttl = cfg_.cache_ttl;
  req.effort = tier->effort;
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
  (void)journal->step("start", json{{"role", to_string(spec.role)},
                                    {"block", spec.block},
                                    {"tier", spec.tier},
                                    {"system_tokens_est", core::estimate_tokens(spec.system_text)},
                                    {"context_tokens_est", core::estimate_tokens(spec.context_text)},
                                    {"task", clip(spec.task_text, 4000)}}
                                   .dump());

  bool nudged = false;
  auto stop_now = [&] {
    if (!stop_seen) stop_seen = journal->stop_requested();  // unthrottled between steps
    return stop_seen || core::cancelled();
  };
  while (true) {
    if (stop_now()) {
      res.outcome = "cancelled";
      (void)journal->step("cancelled", json{{"message", "stopped by the user"}}.dump());
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
      (void)journal->step("cancelled", json{{"message", "stopped by the user"}}.dump());
      break;
    }
    if (!resp) {
      res.outcome = "error";
      res.error = resp.error().message;
      (void)journal->step("error", json{{"message", res.error}}.dump());
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
                                           {"cache_write", resp->usage.cache_write_tokens},
                                           {"cache_read", resp->usage.cache_read_tokens}}},
                                         {"content", content}}
                                        .dump());
    auto text = resp->text();
    if (!text.empty()) res.final_text = text;
    req.messages.push_back({"assistant", resp->content});
    long long used = res.tokens.input + res.tokens.output + res.tokens.cache_write() + res.tokens.cache_read;
    if (resp->stop_reason == "refusal") {
      res.outcome = "refusal";
      break;
    }
    auto uses = resp->tool_uses();
    if (uses.empty()) {
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
                                      .dump());
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
    if (used > spec.max_tokens) {
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
  (void)journal->step("end", json{{"outcome", res.outcome}, {"terminal", res.terminal_tool}, {"reads", res.reads}}.dump());
  auto row = journal->finish(res.tokens, res.cost, res.ms, res.steps, outcome, idx_);
  if (!row) spdlog::warn("agents: cannot finish run journal: {}", row.error().message);
  spdlog::info("agents: {} {} tier={} steps={} outcome={} in={} out={} cache_read={} usd={:.4f}", to_string(spec.role),
               spec.block, spec.tier, res.steps, outcome, res.tokens.input, res.tokens.output, res.tokens.cache_read,
               res.cost);
  return res;
}

}  // namespace kk::agents
