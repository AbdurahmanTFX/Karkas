#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <charconv>
#include <cstdlib>
#include <random>
#include <thread>

#include "kk/core/text.hpp"
#include "kk/llm/provider.hpp"

namespace kk::llm {

ContentBlock ContentBlock::make_text(std::string t, bool cache) {
  ContentBlock b;
  b.type = Type::Text;
  b.text = std::move(t);
  b.cache = cache;
  return b;
}

ContentBlock ContentBlock::make_tool_result(std::string id, std::string content, bool is_error) {
  ContentBlock b;
  b.type = Type::ToolResult;
  b.tool_use_id = std::move(id);
  b.text = std::move(content);
  b.is_error = is_error;
  return b;
}

Usage& Usage::operator+=(const Usage& o) {
  input_tokens += o.input_tokens;
  output_tokens += o.output_tokens;
  cache_write_tokens += o.cache_write_tokens;
  cache_write_1h_tokens += o.cache_write_1h_tokens;
  cache_read_tokens += o.cache_read_tokens;
  return *this;
}

std::string Response::text() const {
  std::string out;
  for (const auto& b : content) {
    if (b.type == ContentBlock::Type::Text) {
      if (!out.empty()) out += "\n";
      out += b.text;
    }
  }
  return out;
}

std::vector<const ContentBlock*> Response::tool_uses() const {
  std::vector<const ContentBlock*> out;
  for (const auto& b : content) {
    if (b.type == ContentBlock::Type::ToolUse) out.push_back(&b);
  }
  return out;
}

kk::Result<HttpResponse> with_retries(const RetryPolicy& policy, const std::function<kk::Result<HttpResponse>()>& call,
                                      int* attempts) {
  static thread_local std::mt19937 rng{std::random_device{}()};
  kk::Result<HttpResponse> r = kk::fail("http", "no attempt made");
  for (int attempt = 1; attempt <= policy.max_attempts; ++attempt) {
    if (attempts != nullptr) *attempts = attempt;
    r = call();
    bool retryable = (!r && r.error().code == "http") || (r && (r->status == 429 || (r->status >= 500 && r->status <= 599)));
    if (!retryable || attempt == policy.max_attempts) return r;
    std::chrono::milliseconds delay = policy.base_delay * (1LL << (attempt - 1));
    delay = std::min(delay, policy.max_delay);
    if (r) {
      auto it = r->headers.find("retry-after");
      if (it != r->headers.end()) {
        double secs = 0;
        auto [p, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), secs);
        if (ec == std::errc()) {
          delay = std::min(std::chrono::milliseconds(static_cast<long long>(secs * 1000.0)), policy.max_retry_after);
        }
      }
    }
    if (policy.jitter) delay += std::chrono::milliseconds(std::uniform_int_distribution<int>(0, 250)(rng));
    spdlog::warn("llm: {} (attempt {}/{}), retrying in {} ms", r ? fmt::format("HTTP {}", r->status) : r.error().message,
                 attempt, policy.max_attempts, delay.count());
    if (policy.sleep) {
      policy.sleep(delay);
    } else {
      std::this_thread::sleep_for(delay);
    }
  }
  return r;
}

bool supports_default_fallback(std::string_view model) {
  return model.starts_with("claude-opus-5") || model.starts_with("claude-sonnet-5-5") ||
         model.starts_with("claude-fable-5");
}

kk::Result<std::unique_ptr<IProvider>> make_provider(const core::ModelTier& tier, const core::Config& cfg,
                                                     IHttpTransport& http) {
  if (tier.model.empty()) return kk::fail("config", "model tier has no model name (set [models.<tier>] model)");
  if (tier.provider == "anthropic") {
    AnthropicOptions o;
    if (const char* key = std::getenv("ANTHROPIC_API_KEY")) o.api_key = key;
    if (!tier.base_url.empty()) o.base_url = tier.base_url;
    o.timeout_s = cfg.request_timeout_s;
    return std::unique_ptr<IProvider>(std::make_unique<AnthropicProvider>(http, o));
  }
  if (tier.provider == "openai_compat") {
    OpenAIOptions o;
    if (const char* key = std::getenv("OPENAI_API_KEY")) o.api_key = key;
    if (!tier.base_url.empty()) o.base_url = tier.base_url;
    o.timeout_s = cfg.request_timeout_s;
    return std::unique_ptr<IProvider>(std::make_unique<OpenAICompatProvider>(http, o));
  }
  return kk::fail("config", fmt::format("unknown provider '{}' (anthropic | openai_compat)", tier.provider));
}

}  // namespace kk::llm
