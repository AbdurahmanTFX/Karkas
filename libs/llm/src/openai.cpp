// OpenAI-compatible chat completions client (local models through Ollama etc., 9.8):
// POST {base_url}/chat/completions with function calling.
#include <fmt/format.h>

#include <nlohmann/json.hpp>

#include "kk/core/text.hpp"
#include "kk/llm/provider.hpp"

namespace kk::llm {

using json = nlohmann::json;

OpenAICompatProvider::OpenAICompatProvider(IHttpTransport& http, OpenAIOptions opts) : http_(http), opts_(std::move(opts)) {}

std::string OpenAICompatProvider::request_json(const Request& req) {
  json j;
  j["model"] = req.model;
  j["max_tokens"] = req.max_tokens;
  json msgs = json::array();
  std::string sys;
  for (const auto& b : req.system) {
    if (b.type == ContentBlock::Type::Text) sys += (sys.empty() ? "" : "\n\n") + b.text;
  }
  if (!sys.empty()) msgs.push_back({{"role", "system"}, {"content", sys}});
  for (const auto& m : req.messages) {
    if (m.role == "assistant") {
      json a = {{"role", "assistant"}};
      std::string text;
      json calls = json::array();
      for (const auto& b : m.content) {
        if (b.type == ContentBlock::Type::Text) text += b.text;
        if (b.type == ContentBlock::Type::ToolUse) {
          calls.push_back({{"id", b.id}, {"type", "function"}, {"function", {{"name", b.name}, {"arguments", b.input_json}}}});
        }
      }
      a["content"] = text.empty() ? json(nullptr) : json(text);
      if (!calls.empty()) a["tool_calls"] = calls;
      msgs.push_back(std::move(a));
      continue;
    }
    std::string text;
    for (const auto& b : m.content) {
      if (b.type == ContentBlock::Type::ToolResult) {
        msgs.push_back({{"role", "tool"}, {"tool_call_id", b.tool_use_id}, {"content", (b.is_error ? "ERROR: " : "") + b.text}});
      } else if (b.type == ContentBlock::Type::Text) {
        text += (text.empty() ? "" : "\n\n") + b.text;
      }
    }
    if (!text.empty()) msgs.push_back({{"role", "user"}, {"content", text}});
  }
  j["messages"] = std::move(msgs);
  if (!req.tools.empty()) {
    json tools = json::array();
    for (const auto& t : req.tools) {
      json params;
      try {
        params = json::parse(t.input_schema_json);
      } catch (const json::exception&) {
        params = json::object();
      }
      tools.push_back({{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", params}}}});
    }
    j["tools"] = std::move(tools);
  }
  return j.dump();
}

kk::Result<Response> OpenAICompatProvider::parse_response(const std::string& body) {
  json j;
  try {
    j = json::parse(body);
  } catch (const json::exception& e) {
    return kk::fail("llm", std::string("invalid JSON: ") + e.what());
  }
  if (j.contains("error")) {
    const auto& e = j["error"];
    return kk::fail("llm", e.is_object() ? e.value("message", e.dump()) : e.dump());
  }
  Response r;
  r.id = j.value("id", "");
  r.model = j.value("model", "");
  if (!j.contains("choices") || j["choices"].empty()) return kk::fail("llm", "response has no choices");
  const auto& ch = j["choices"][0];
  std::string finish = ch.value("finish_reason", "");
  r.stop_reason = finish == "tool_calls" ? "tool_use" : finish == "length" ? "max_tokens" : "end_turn";
  const auto& msg = ch.value("message", json::object());
  if (msg.contains("content") && msg["content"].is_string() && !msg["content"].get<std::string>().empty()) {
    r.content.push_back(ContentBlock::make_text(msg["content"].get<std::string>()));
  }
  for (const auto& c : msg.value("tool_calls", json::array())) {
    ContentBlock b;
    b.type = ContentBlock::Type::ToolUse;
    b.id = c.value("id", "");
    b.name = c.contains("function") ? c["function"].value("name", "") : "";
    std::string args = c.contains("function") ? c["function"].value("arguments", "{}") : "{}";
    try {
      b.input_json = json::parse(args.empty() ? "{}" : args).dump();
    } catch (const json::exception&) {
      b.input_json = json{{"_unparsed", args}}.dump();  // the agent loop reports it as a tool error
    }
    r.content.push_back(std::move(b));
  }
  if (!r.tool_uses().empty()) r.stop_reason = "tool_use";
  if (j.contains("usage")) {
    const auto& u = j["usage"];
    r.usage.input_tokens = u.value("prompt_tokens", std::int64_t{0});
    r.usage.output_tokens = u.value("completion_tokens", std::int64_t{0});
    if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) {
      r.usage.cache_read_tokens = u["prompt_tokens_details"].value("cached_tokens", std::int64_t{0});
      r.usage.input_tokens -= r.usage.cache_read_tokens;
    }
  }
  return r;
}

kk::Result<Response> OpenAICompatProvider::send(const Request& req) {
  HttpRequest h;
  h.url = opts_.base_url + "/chat/completions";
  h.body = request_json(req);
  h.timeout_s = opts_.timeout_s;
  h.headers = {{"content-type", "application/json"}};
  if (!opts_.api_key.empty()) h.headers.emplace_back("authorization", "Bearer " + opts_.api_key);
  int attempts = 0;
  auto r = with_retries(opts_.retry, [&] { return http_.post(h); }, &attempts);
  if (!r) return std::unexpected(r.error());
  if (r->status != 200) return kk::fail("http", fmt::format("HTTP {}: {}", r->status, r->body.substr(0, 300)));
  auto resp = parse_response(r->body);
  if (resp) resp->http_attempts = attempts;
  return resp;
}

kk::Result<std::int64_t> OpenAICompatProvider::count_tokens(const Request& req) {
  return static_cast<std::int64_t>(core::estimate_tokens(request_json(req)));
}

}  // namespace kk::llm
