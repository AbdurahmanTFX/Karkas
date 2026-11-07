// Anthropic Messages API client (9.8): POST /v1/messages, tool use, cache_control
// markers, usage, retries. Model names and prices live only in the config.
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "kk/llm/provider.hpp"

namespace kk::llm {

using json = nlohmann::json;

namespace {

constexpr int kMaxCacheMarks = 4;
constexpr const char* kFallbackBeta = "server-side-fallback-2026-07-01";

json cache_control(const std::string& ttl) {
  json c = {{"type", "ephemeral"}};
  if (ttl == "1h") c["ttl"] = "1h";
  return c;
}

json parse_or_object(const std::string& s) {
  try {
    return json::parse(s.empty() ? "{}" : s);
  } catch (const json::exception&) {
    return json::object();
  }
}

json block_json(const ContentBlock& b, const std::string& ttl) {
  json j;
  if (!b.raw_json.empty()) {
    j = parse_or_object(b.raw_json);  // verbatim replay (thinking signatures etc.)
  } else {
    switch (b.type) {
      case ContentBlock::Type::Text: j = {{"type", "text"}, {"text", b.text}}; break;
      case ContentBlock::Type::ToolUse:
        j = {{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", parse_or_object(b.input_json)}};
        break;
      case ContentBlock::Type::ToolResult:
        j = {{"type", "tool_result"}, {"tool_use_id", b.tool_use_id}, {"content", b.text}};
        if (b.is_error) j["is_error"] = true;
        break;
      case ContentBlock::Type::Other: return nullptr;
    }
  }
  std::string t = j.value("type", "");
  if (b.cache && (t == "text" || t == "tool_use" || t == "tool_result")) j["cache_control"] = cache_control(ttl);
  return j;
}

// At most 4 markers per request (9.6): keep the first (stable system/tools prefix)
// and the last three (moving conversation markers).
Request limit_cache_marks(Request r) {
  std::vector<bool*> marks;
  for (auto& t : r.tools) {
    if (t.cache) marks.push_back(&t.cache);
  }
  for (auto& b : r.system) {
    if (b.cache) marks.push_back(&b.cache);
  }
  for (auto& m : r.messages) {
    for (auto& b : m.content) {
      if (b.cache) marks.push_back(&b.cache);
    }
  }
  if (static_cast<int>(marks.size()) > kMaxCacheMarks) {
    for (std::size_t i = 1; i + (kMaxCacheMarks - 1) < marks.size(); ++i) *marks[i] = false;
  }
  return r;
}

std::string api_error_message(const HttpResponse& r) {
  try {
    auto j = json::parse(r.body);
    if (j.contains("error")) {
      return fmt::format("HTTP {} {}: {}", r.status, j["error"].value("type", ""), j["error"].value("message", ""));
    }
  } catch (const json::exception&) {
  }
  return fmt::format("HTTP {}: {}", r.status, r.body.substr(0, 300));
}

}  // namespace

AnthropicProvider::AnthropicProvider(IHttpTransport& http, AnthropicOptions opts) : http_(http), opts_(std::move(opts)) {}

std::string AnthropicProvider::request_json(const Request& in, bool for_count) {
  Request req = limit_cache_marks(in);
  json j;
  j["model"] = req.model;
  if (!for_count) j["max_tokens"] = req.max_tokens;
  if (!req.tools.empty()) {
    json tools = json::array();
    for (const auto& t : req.tools) {
      json tj = {{"name", t.name}, {"description", t.description}, {"input_schema", parse_or_object(t.input_schema_json)}};
      if (t.cache) tj["cache_control"] = cache_control(req.cache_ttl);
      tools.push_back(std::move(tj));
    }
    j["tools"] = std::move(tools);
  }
  if (!req.system.empty()) {
    json sys = json::array();
    for (const auto& b : req.system) {
      auto bj = block_json(b, req.cache_ttl);
      if (!bj.is_null()) sys.push_back(std::move(bj));
    }
    j["system"] = std::move(sys);
  }
  json msgs = json::array();
  for (const auto& m : req.messages) {
    json content = json::array();
    for (const auto& b : m.content) {
      auto bj = block_json(b, req.cache_ttl);
      if (!bj.is_null()) content.push_back(std::move(bj));
    }
    msgs.push_back(json{{"role", m.role}, {"content", std::move(content)}});
  }
  j["messages"] = std::move(msgs);
  if (!for_count) {
    if (!req.effort.empty()) j["output_config"] = {{"effort", req.effort}};
    if (!req.fallbacks.empty()) j["fallbacks"] = req.fallbacks;
  }
  // Sampling parameters (temperature/top_p/top_k) are never sent: API version
  // 2026-10-21 rejects them (docs/adr/0005).
  return j.dump();
}

kk::Result<Response> AnthropicProvider::parse_response(const std::string& body) {
  json j;
  try {
    j = json::parse(body);
  } catch (const json::exception& e) {
    return kk::fail("llm", std::string("invalid JSON from the API: ") + e.what());
  }
  if (j.value("type", "") == "error") {
    return kk::fail("llm", fmt::format("{}: {}", j["error"].value("type", ""), j["error"].value("message", "")));
  }
  Response r;
  r.id = j.value("id", "");
  r.model = j.value("model", "");
  r.stop_reason = j.value("stop_reason", "");
  for (const auto& c : j.value("content", json::array())) {
    ContentBlock b;
    std::string t = c.value("type", "");
    b.raw_json = c.dump();
    if (t == "text") {
      b.type = ContentBlock::Type::Text;
      b.text = c.value("text", "");
    } else if (t == "tool_use") {
      b.type = ContentBlock::Type::ToolUse;
      b.id = c.value("id", "");
      b.name = c.value("name", "");
      b.input_json = c.contains("input") ? c["input"].dump() : "{}";
    } else {
      b.type = ContentBlock::Type::Other;
    }
    r.content.push_back(std::move(b));
  }
  if (j.contains("usage")) {
    const auto& u = j["usage"];
    auto num = [&](const char* k) { return u.contains(k) && u[k].is_number() ? u[k].get<std::int64_t>() : 0; };
    r.usage.input_tokens = num("input_tokens");
    r.usage.output_tokens = num("output_tokens");
    r.usage.cache_write_tokens = num("cache_creation_input_tokens");
    r.usage.cache_read_tokens = num("cache_read_input_tokens");
    if (u.contains("cache_creation") && u["cache_creation"].is_object()) {
      r.usage.cache_write_1h_tokens = u["cache_creation"].value("ephemeral_1h_input_tokens", std::int64_t{0});
    }
  }
  return r;
}

kk::Result<HttpResponse> AnthropicProvider::post(const std::string& path, const std::string& body, bool beta_fallback) {
  HttpRequest req;
  req.url = opts_.base_url + path;
  req.body = body;
  req.timeout_s = opts_.timeout_s;
  req.headers = {{"content-type", "application/json"},
                 {"x-api-key", opts_.api_key},
                 {"anthropic-version", opts_.api_version}};
  if (beta_fallback) req.headers.emplace_back("anthropic-beta", kFallbackBeta);
  return http_.post(req);
}

kk::Result<Response> AnthropicProvider::send(const Request& req) {
  if (opts_.api_key.empty()) return kk::fail("llm", "ANTHROPIC_API_KEY is not set");
  auto body = request_json(req);
  int attempts = 0;
  auto r = with_retries(opts_.retry, [&] { return post("/v1/messages", body, !req.fallbacks.empty()); }, &attempts);
  if (!r) return std::unexpected(r.error());
  if (r->status != 200) {
    auto msg = api_error_message(*r);
    spdlog::error("llm: anthropic {} after {} attempt(s)", msg, attempts);
    return kk::fail("http", msg);
  }
  auto resp = parse_response(r->body);
  if (resp) resp->http_attempts = attempts;
  return resp;
}

kk::Result<std::int64_t> AnthropicProvider::count_tokens(const Request& req) {
  if (opts_.api_key.empty()) return kk::fail("llm", "ANTHROPIC_API_KEY is not set");
  auto body = request_json(req, true);
  auto r = with_retries(opts_.retry, [&] { return post("/v1/messages/count_tokens", body, false); });
  if (!r) return std::unexpected(r.error());
  if (r->status != 200) return kk::fail("http", api_error_message(*r));
  try {
    return json::parse(r->body).at("input_tokens").get<std::int64_t>();
  } catch (const json::exception& e) {
    return kk::fail("llm", std::string("bad count_tokens response: ") + e.what());
  }
}

}  // namespace kk::llm
