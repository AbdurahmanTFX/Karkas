#pragma once
// Provider-neutral request/response types for model calls (9.8). JSON payloads are
// carried as text so that headers stay light (no nlohmann/json.hpp here).
#include <cstdint>
#include <string>
#include <vector>

#include "kk/core/error.hpp"

namespace kk::llm {

struct ContentBlock {
  enum class Type { Text, ToolUse, ToolResult, Other };
  Type type = Type::Text;
  std::string text;               // Text; ToolResult content
  std::string id, name;           // ToolUse
  std::string input_json = "{}";  // ToolUse input (JSON text)
  std::string tool_use_id;        // ToolResult
  bool is_error = false;          // ToolResult
  bool cache = false;             // put a cache_control marker on this block (9.6)
  // Exact JSON of a block returned by the API (thinking blocks, signatures, ...).
  // When set it is sent back verbatim: assistant turns are never rewritten.
  std::string raw_json;

  static ContentBlock make_text(std::string t, bool cache = false);
  static ContentBlock make_tool_result(std::string id, std::string content, bool is_error = false);
};

struct Message {
  std::string role;  // user | assistant
  std::vector<ContentBlock> content;
};

struct ToolDef {
  std::string name;
  std::string description;
  std::string input_schema_json;  // JSON Schema (Claude API: input_schema)
  bool cache = false;             // cache_control on this tool definition
};

struct Request {
  std::string model;
  int max_tokens = 16000;  // output limit of one answer; 0 = the provider's own maximum
  std::vector<ContentBlock> system;
  std::vector<ToolDef> tools;
  std::vector<Message> messages;
  std::string cache_ttl = "5m";  // "5m" | "1h"
  std::string effort;            // output_config.effort; "" = model default
  std::string fallbacks;         // "default" = server-side refusal fallback; "" = off
  // The agent run this request belongs to, as JSON text ({"journal", "ticket", "role", "block",
  // "tier", "subagent"}): providers ignore it; the MCP bridge shows it to the client (ADR 0024).
  std::string meta_json;
};

struct Usage {
  std::int64_t input_tokens = 0;
  std::int64_t output_tokens = 0;
  std::int64_t cache_write_tokens = 0;     // cache_creation_input_tokens (all TTLs)
  std::int64_t cache_write_1h_tokens = 0;  // part of cache_write_tokens written with ttl=1h
  std::int64_t cache_read_tokens = 0;      // cache_read_input_tokens
  std::int64_t reasoning_tokens = 0;       // part of output_tokens spent on hidden reasoning (if reported)
  Usage& operator+=(const Usage& o);
};

struct Response {
  std::string id;
  std::string model;
  std::string stop_reason;  // end_turn | tool_use | max_tokens | refusal | ...
  std::vector<ContentBlock> content;
  Usage usage;
  int http_attempts = 1;

  [[nodiscard]] std::string text() const;  // all text blocks joined
  [[nodiscard]] std::vector<const ContentBlock*> tool_uses() const;
};

class IProvider {
 public:
  virtual ~IProvider() = default;
  virtual kk::Result<Response> send(const Request& req) = 0;
  virtual kk::Result<std::int64_t> count_tokens(const Request& req) = 0;
  [[nodiscard]] virtual std::string name() const = 0;  // anthropic | openai_compat | ...
};

}  // namespace kk::llm
