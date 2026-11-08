#pragma once
// Model clients (9.8): Anthropic Messages API and OpenAI-compatible chat completions.
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "kk/core/config.hpp"
#include "kk/llm/http.hpp"
#include "kk/llm/types.hpp"

namespace kk::llm {

struct RetryPolicy {
  int max_attempts = 5;  // total attempts for 429, 5xx, 529 and transport errors
  std::chrono::milliseconds base_delay{1000};
  std::chrono::milliseconds max_delay{32000};
  std::chrono::milliseconds max_retry_after{60000};  // cap for the retry-after header
  bool jitter = true;
  // Injected for tests; defaults to std::this_thread::sleep_for.
  std::function<void(std::chrono::milliseconds)> sleep;
};

struct AnthropicOptions {
  std::string key_env = "ANTHROPIC_API_KEY";  // where api_key came from (for the error message)
  std::string api_key;  // from the environment only (6.5): key_env
  std::string base_url = "https://api.anthropic.com";
  std::string api_version = "2026-10-21";
  int timeout_s = 300;
  RetryPolicy retry;
};

class AnthropicProvider final : public IProvider {
 public:
  AnthropicProvider(IHttpTransport& http, AnthropicOptions opts);
  kk::Result<Response> send(const Request& req) override;
  kk::Result<std::int64_t> count_tokens(const Request& req) override;
  [[nodiscard]] std::string name() const override { return "anthropic"; }

  // Request body as sent (exposed for tests).
  static std::string request_json(const Request& req, bool for_count = false);
  static kk::Result<Response> parse_response(const std::string& body);

 private:
  kk::Result<HttpResponse> post(const std::string& path, const std::string& body, bool beta_fallback);
  IHttpTransport& http_;
  AnthropicOptions opts_;
};

struct OpenAIOptions {
  std::string api_key;  // optional (local servers usually need none)
  std::string base_url = "http://localhost:11434/v1";
  int timeout_s = 300;
  RetryPolicy retry;
};

class OpenAICompatProvider final : public IProvider {
 public:
  OpenAICompatProvider(IHttpTransport& http, OpenAIOptions opts);
  kk::Result<Response> send(const Request& req) override;
  kk::Result<std::int64_t> count_tokens(const Request& req) override;  // local estimate
  [[nodiscard]] std::string name() const override { return "openai_compat"; }

  static std::string request_json(const Request& req);
  static kk::Result<Response> parse_response(const std::string& body);

 private:
  IHttpTransport& http_;
  OpenAIOptions opts_;
};

// Retries `call` on 429, 500-599, 529 and transport errors with exponential backoff
// honouring retry-after (seconds); 4xx others are returned at once.
kk::Result<HttpResponse> with_retries(const RetryPolicy& policy, const std::function<kk::Result<HttpResponse>()>& call,
                                      int* attempts = nullptr);

// Builds the provider for a model tier from the config. The API key comes from the
// environment (ANTHROPIC_API_KEY; OPENAI_API_KEY for openai_compat, optional).
kk::Result<std::unique_ptr<IProvider>> make_provider(const core::ModelTier& tier, const core::Config& cfg,
                                                     IHttpTransport& http);

// Server-side refusal fallback applies to these model families (Claude API only).
bool supports_default_fallback(std::string_view model);

}  // namespace kk::llm
