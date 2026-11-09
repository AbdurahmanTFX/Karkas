#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "kk/core/cancel.hpp"
#include "kk/core/text.hpp"
#include "kk/llm/provider.hpp"
#include "kk/test/support.hpp"

using namespace kk;
using json = nlohmann::json;

namespace {

std::filesystem::path cassette(const char* name) { return test::fixture("llm") / name; }

struct Sleeps {
  std::vector<long long> ms;
  llm::RetryPolicy policy() {
    llm::RetryPolicy p;
    p.jitter = false;
    p.sleep = [this](std::chrono::milliseconds d) { ms.push_back(d.count()); };
    return p;
  }
};

llm::AnthropicOptions opts(Sleeps& s) {
  llm::AnthropicOptions o;
  o.api_key = "sk-test-secret";
  o.retry = s.policy();
  return o;
}

llm::Request simple_request() {
  llm::Request r;
  r.model = "claude-haiku-5-5";
  r.max_tokens = 1000;
  r.system = {llm::ContentBlock::make_text("You are the WORKER.", true)};
  r.messages = {{"user", {llm::ContentBlock::make_text("hi")}}};
  return r;
}

// Captures requests and answers with scripted responses.
struct Capture final : llm::IHttpTransport {
  std::vector<llm::HttpRequest> seen;
  std::vector<llm::HttpResponse> answers;
  kk::Result<llm::HttpResponse> post(const llm::HttpRequest& r) override {
    seen.push_back(r);
    if (answers.empty()) return llm::HttpResponse{200, {}, R"({"type":"message","content":[],"stop_reason":"end_turn","usage":{}})"};
    auto a = answers.front();
    answers.erase(answers.begin());
    return a;
  }
};

}  // namespace

TEST_CASE("llm: plain text response and usage (replay)") {
  Sleeps s;
  llm::ReplayTransport t(cassette("text.jsonl"));
  llm::AnthropicProvider p(t, opts(s));
  auto r = p.send(simple_request());
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(r->text() == "Hello from the ring.");
  CHECK(r->stop_reason == "end_turn");
  CHECK(r->usage.input_tokens == 12);
  CHECK(r->usage.output_tokens == 6);
  CHECK(r->usage.cache_write_tokens == 2048);
  CHECK(r->usage.cache_read_tokens == 0);
  CHECK(r->http_attempts == 1);
  CHECK(s.ms.empty());
}

TEST_CASE("llm: tool use response keeps raw blocks for verbatim replay") {
  Sleeps s;
  llm::ReplayTransport t(cassette("tool_use.jsonl"));
  llm::AnthropicProvider p(t, opts(s));
  auto r = p.send(simple_request());
  REQUIRE(r);
  CHECK(r->stop_reason == "tool_use");
  REQUIRE(r->tool_uses().size() == 1);
  const auto* tu = r->tool_uses()[0];
  CHECK(tu->id == "toolu_01");
  CHECK(tu->name == "read_file");
  CHECK(json::parse(tu->input_json)["path"] == "impl/ring.cpp");
  CHECK(r->content[0].type == llm::ContentBlock::Type::Other);  // thinking block
  CHECK(r->usage.cache_read_tokens == 1900);

  // The assistant turn goes back unchanged, with the tool result in the next user turn.
  llm::Request next = simple_request();
  next.messages.push_back({"assistant", r->content});
  next.messages.push_back({"user", {llm::ContentBlock::make_tool_result("toolu_01", "int x;", false)}});
  auto body = json::parse(llm::AnthropicProvider::request_json(next));
  const auto& asst = body["messages"][1]["content"];
  CHECK(asst[0]["type"] == "thinking");
  CHECK(asst[0]["signature"] == "sig-abc");
  CHECK(asst[2]["input"]["path"] == "impl/ring.cpp");
  const auto& tr = body["messages"][2]["content"][0];
  CHECK(tr["type"] == "tool_result");
  CHECK(tr["tool_use_id"] == "toolu_01");
  CHECK(tr["content"] == "int x;");
  CHECK_FALSE(tr.contains("is_error"));
}

TEST_CASE("llm: 429 honours retry-after, then succeeds") {
  Sleeps s;
  llm::ReplayTransport t(cassette("retry_429.jsonl"));
  llm::AnthropicProvider p(t, opts(s));
  auto r = p.send(simple_request());
  REQUIRE(r);
  CHECK(r->http_attempts == 2);
  CHECK(s.ms == std::vector<long long>{2000});
}

TEST_CASE("llm: 500, 529 and a transport error are retried with exponential backoff") {
  Sleeps s;
  llm::ReplayTransport t(cassette("retry_5xx.jsonl"));
  llm::AnthropicProvider p(t, opts(s));
  auto r = p.send(simple_request());
  REQUIRE(r);
  CHECK(r->http_attempts == 4);
  CHECK(s.ms == std::vector<long long>{1000, 2000, 4000});
}

TEST_CASE("llm: 400 is not retried") {
  Sleeps s;
  llm::ReplayTransport t(cassette("bad_request.jsonl"));
  llm::AnthropicProvider p(t, opts(s));
  auto r = p.send(simple_request());
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "invalid_request_error"));
  CHECK(core::contains(r.error().message, "messages: field required"));
  CHECK(t.remaining() == 1);
  CHECK(s.ms.empty());
}

TEST_CASE("llm: persistent 529 gives up after 5 attempts") {
  Sleeps s;
  llm::ReplayTransport t(cassette("overloaded.jsonl"));
  llm::AnthropicProvider p(t, opts(s));
  auto r = p.send(simple_request());
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "529"));
  CHECK(s.ms.size() == 4);
  CHECK(t.remaining() == 1);
}

TEST_CASE("llm: request shape — headers, cache markers, no sampling parameters") {
  Sleeps s;
  Capture cap;
  llm::AnthropicProvider p(cap, opts(s));
  llm::Request r = simple_request();
  r.cache_ttl = "1h";
  r.effort = "medium";
  r.fallbacks = "default";
  r.tools = {{"read_file", "Read a file", R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})", false}};
  for (int i = 0; i < 6; ++i) r.messages.push_back({i % 2 == 0 ? "assistant" : "user", {llm::ContentBlock::make_text("m", true)}});
  REQUIRE(p.send(r));
  REQUIRE(cap.seen.size() == 1);
  const auto& h = cap.seen[0];
  CHECK(h.url == "https://api.anthropic.com/v1/messages");
  auto header = [&](const std::string& k) {
    for (const auto& [n, v] : h.headers) {
      if (n == k) return v;
    }
    return std::string();
  };
  CHECK(header("anthropic-version") == "2026-10-21");
  CHECK(header("x-api-key") == "sk-test-secret");
  CHECK(header("anthropic-beta") == "server-side-fallback-2026-07-01");
  auto body = json::parse(h.body);
  CHECK(body["model"] == "claude-haiku-5-5");
  CHECK(body["max_tokens"] == 1000);
  CHECK_FALSE(body.contains("temperature"));
  CHECK_FALSE(body.contains("top_p"));
  CHECK_FALSE(body.contains("top_k"));
  CHECK(body["output_config"]["effort"] == "medium");
  CHECK(body["fallbacks"] == "default");
  CHECK(body["tools"][0]["input_schema"]["required"][0] == "path");
  CHECK(body["system"][0]["cache_control"]["type"] == "ephemeral");
  CHECK(body["system"][0]["cache_control"]["ttl"] == "1h");
  int marks = 0;
  auto count = [&](const json& blocks) {
    for (const auto& b : blocks) marks += b.contains("cache_control") ? 1 : 0;
  };
  count(body["system"]);
  for (const auto& m : body["messages"]) count(m["content"]);
  CHECK(marks == 4);  // first kept, the last three moved forward
  CHECK(body["messages"].back()["content"][0].contains("cache_control"));
  CHECK_FALSE(body["messages"][1]["content"][0].contains("cache_control"));
}

TEST_CASE("llm: missing API key fails before any request") {
  Capture cap;
  llm::AnthropicOptions o;
  llm::AnthropicProvider p(cap, o);
  auto r = p.send(simple_request());
  REQUIRE_FALSE(r);
  CHECK(r.error().message == "ANTHROPIC_API_KEY is not set");
  CHECK(cap.seen.empty());
}

TEST_CASE("llm: count_tokens endpoint") {
  Sleeps s;
  llm::ReplayTransport t(cassette("count.jsonl"));
  llm::AnthropicProvider p(t, opts(s));
  auto n = p.count_tokens(simple_request());
  REQUIRE(n);
  CHECK(*n == 731);
  auto body = json::parse(t.requests().at(0));
  CHECK_FALSE(body.contains("max_tokens"));
}

TEST_CASE("llm: OpenAI-compatible translation") {
  Sleeps s;
  llm::ReplayTransport t(cassette("openai.jsonl"));
  llm::OpenAIOptions o;
  o.retry = s.policy();
  llm::OpenAICompatProvider p(t, o);
  llm::Request r = simple_request();
  r.model = "qwen";
  r.tools = {{"report", "Finish", R"({"type":"object","properties":{"status":{"type":"string"}}})", false}};
  llm::ContentBlock tu;
  tu.type = llm::ContentBlock::Type::ToolUse;
  tu.id = "call_0";
  tu.name = "read_file";
  tu.input_json = R"({"path":"a"})";
  r.messages.push_back({"assistant", {llm::ContentBlock::make_text("reading"), tu}});
  r.messages.push_back({"user", {llm::ContentBlock::make_tool_result("call_0", "file text", true)}});
  auto resp = p.send(r);
  REQUIRE_MESSAGE(resp, (resp ? "" : resp.error().str()));
  CHECK(resp->stop_reason == "tool_use");
  REQUIRE(resp->tool_uses().size() == 1);
  CHECK(resp->tool_uses()[0]->name == "report");
  CHECK(json::parse(resp->tool_uses()[0]->input_json)["status"] == "done");
  CHECK(resp->usage.input_tokens == 60);
  CHECK(resp->usage.cache_read_tokens == 40);
  auto body = json::parse(t.requests().at(0));
  CHECK(body["messages"][0]["role"] == "system");
  CHECK(body["messages"][2]["tool_calls"][0]["function"]["name"] == "read_file");
  CHECK(body["messages"][3]["role"] == "tool");
  CHECK(body["messages"][3]["content"] == "ERROR: file text");
  CHECK(body["tools"][0]["type"] == "function");
}

TEST_CASE("llm: recording never writes the API key") {
  test::TempDir d{"rec"};
  Capture cap;
  cap.answers.push_back({200, {{"request-id", "req_1"}}, R"({"type":"message","content":[{"type":"text","text":"x"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})"});
  llm::RecordingTransport rec(cap, d / "c.jsonl");
  Sleeps s;
  llm::AnthropicProvider p(rec, opts(s));
  REQUIRE(p.send(simple_request()));
  auto text = test::read(d / "c.jsonl");
  CHECK_FALSE(core::contains(text, "sk-test-secret"));
  CHECK(core::contains(text, "anthropic-version"));
  // The recording replays.
  llm::ReplayTransport replay(d / "c.jsonl", true);
  llm::AnthropicProvider p2(replay, opts(s));
  auto r = p2.send(simple_request());
  REQUIRE(r);
  CHECK(r->text() == "x");
  // Strict replay rejects a different request.
  llm::ReplayTransport strict(d / "c.jsonl", true);
  llm::AnthropicProvider p3(strict, opts(s));
  auto other = simple_request();
  other.max_tokens = 5;
  CHECK_FALSE(p3.send(other));
}

core::ModelTier tier(std::string provider, std::string model, std::string base) {
  core::ModelTier t;
  t.provider = std::move(provider);
  t.model = std::move(model);
  t.base_url = std::move(base);
  return t;
}

TEST_CASE("llm: provider factory and fallback families") {
  core::Config cfg;
  Capture cap;
  auto bad = llm::make_provider(tier("anthropic", "", ""), cfg, cap);
  CHECK_FALSE(bad);
  auto unknown = llm::make_provider(tier("gemini", "x", ""), cfg, cap);
  CHECK_FALSE(unknown);
  auto local = llm::make_provider(tier("openai_compat", "qwen", "http://localhost:11434/v1"), cfg, cap);
  REQUIRE(local);
  CHECK((*local)->name() == "openai_compat");
  CHECK(llm::supports_default_fallback("claude-opus-5-5"));
  CHECK(llm::supports_default_fallback("claude-sonnet-5-5"));
  CHECK_FALSE(llm::supports_default_fallback("claude-haiku-5-5"));
}

TEST_CASE("llm: a stopped agent does not retry (ADR 0019)") {
  int calls = 0;
  llm::RetryPolicy p;
  p.jitter = false;
  p.sleep = [](std::chrono::milliseconds) { core::request_cancel(); };  // the user stops during the back-off
  auto r = llm::with_retries(p, [&]() -> kk::Result<llm::HttpResponse> {
    ++calls;
    return llm::HttpResponse{529, {}, "overloaded"};
  });
  core::reset_cancel();
  CHECK(calls == 1);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "cancelled");
  // A cancelled transport error is never retried either.
  calls = 0;
  auto r2 = llm::with_retries(p, [&]() -> kk::Result<llm::HttpResponse> {
    ++calls;
    return kk::fail("cancelled", "stopped by the user");
  });
  CHECK(calls == 1);
  CHECK_FALSE(r2);
}
