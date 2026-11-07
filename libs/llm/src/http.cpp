#include "kk/llm/http.hpp"

#include <curl/curl.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::llm {

using json = nlohmann::json;

namespace {

struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

struct Handle {
  CURL* h = nullptr;
  Handle() : h(curl_easy_init()) {}
  ~Handle() {
    if (h != nullptr) curl_easy_cleanup(h);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

std::size_t on_body(char* data, std::size_t size, std::size_t n, void* user) {
  static_cast<std::string*>(user)->append(data, size * n);
  return size * n;
}

std::size_t on_header(char* data, std::size_t size, std::size_t n, void* user) {
  std::string_view line(data, size * n);
  auto colon = line.find(':');
  if (colon != std::string_view::npos) {
    auto key = core::to_lower(core::trim(line.substr(0, colon)));
    auto value = std::string(core::trim(line.substr(colon + 1)));
    (*static_cast<std::map<std::string, std::string>*>(user))[key] = value;
  }
  return size * n;
}

bool secret_header(std::string_view name) {
  auto n = core::to_lower(name);
  return n == "x-api-key" || n == "authorization" || core::contains(n, "token") || core::contains(n, "secret");
}

json body_as_json(const std::string& body) {
  try {
    return json::parse(body);
  } catch (const json::exception&) {
    return body;
  }
}

}  // namespace

CurlTransport::CurlTransport() { static CurlGlobal global; }

kk::Result<HttpResponse> CurlTransport::post(const HttpRequest& req) {
  thread_local Handle handle;  // one connection per thread, reused (keep-alive)
  if (handle.h == nullptr) return kk::fail("http", "curl_easy_init failed");
  CURL* h = handle.h;
  curl_easy_reset(h);
  HttpResponse resp;
  curl_slist* headers = nullptr;
  for (const auto& [k, v] : req.headers) headers = curl_slist_append(headers, (k + ": " + v).c_str());
  curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(req.timeout_s));
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &resp.headers);
  CURLcode rc = curl_easy_perform(h);
  curl_slist_free_all(headers);
  if (rc != CURLE_OK) return kk::fail("http", fmt::format("request failed: {}", curl_easy_strerror(rc)));
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  resp.status = static_cast<int>(status);
  return resp;
}

RecordingTransport::RecordingTransport(IHttpTransport& inner, std::filesystem::path cassette)
    : inner_(inner), cassette_(std::move(cassette)) {}

kk::Result<HttpResponse> RecordingTransport::post(const HttpRequest& req) {
  auto r = inner_.post(req);
  json safe_headers = json::object();
  for (const auto& [k, v] : req.headers) {
    if (!secret_header(k)) safe_headers[core::to_lower(k)] = v;
  }
  json entry = {{"url", req.url}, {"request_headers", safe_headers}, {"request", body_as_json(req.body)}};
  if (r) {
    entry["status"] = r->status;
    json hs = json::object();
    for (const auto& [k, v] : r->headers) {
      if (!secret_header(k)) hs[k] = v;
    }
    entry["headers"] = hs;
    entry["body"] = r->body;
  } else {
    entry["error"] = r.error().message;
  }
  std::lock_guard lk(mu_);
  (void)core::append_file(cassette_, entry.dump() + "\n");
  return r;
}

ReplayTransport::ReplayTransport(std::filesystem::path cassette, bool strict) : strict_(strict) {
  auto text = core::read_file(cassette);
  if (!text) {
    load_error_ = text.error().message;
    return;
  }
  for (const auto& line : core::split(*text, '\n')) {
    if (core::trim(line).empty() || core::trim(line)[0] == '#') continue;
    try {
      auto j = json::parse(line);
      Entry e;
      e.url = j.value("url", "");
      if (j.contains("request")) e.body = j["request"].is_string() ? j["request"].get<std::string>() : j["request"].dump();
      e.response.status = j.value("status", 0);
      if (j.contains("headers")) {
        for (const auto& [k, v] : j["headers"].items()) e.response.headers[core::to_lower(k)] = v.get<std::string>();
      }
      if (j.contains("body")) {
        e.response.body = j["body"].is_string() ? j["body"].get<std::string>() : j["body"].dump();
      }
      e.transport_error = j.value("error", "");
      entries_.push_back(std::move(e));
    } catch (const json::exception& ex) {
      load_error_ = fmt::format("bad cassette line in {}: {}", cassette.string(), ex.what());
      return;
    }
  }
}

std::size_t ReplayTransport::remaining() const {
  std::lock_guard lk(mu_);
  return entries_.size() - next_;
}

kk::Result<HttpResponse> ReplayTransport::post(const HttpRequest& req) {
  std::lock_guard lk(mu_);
  if (!load_error_.empty()) return kk::fail("replay", load_error_);
  seen_.push_back(req.body);
  if (next_ >= entries_.size()) return kk::fail("replay", "cassette exhausted: unexpected request to " + req.url);
  const auto& e = entries_[next_++];
  if (strict_ && !e.body.empty()) {
    bool same = false;
    try {
      same = json::parse(e.body) == json::parse(req.body);
    } catch (const json::exception&) {
      same = e.body == req.body;
    }
    if (!same) return kk::fail("replay", fmt::format("request {} differs from the cassette", next_));
  }
  if (!e.transport_error.empty()) return kk::fail("http", e.transport_error);
  return e.response;
}

std::unique_ptr<IHttpTransport> make_transport_from_env() {
  struct Owning final : IHttpTransport {
    std::unique_ptr<IHttpTransport> inner;
    std::unique_ptr<IHttpTransport> outer;
    kk::Result<HttpResponse> post(const HttpRequest& r) override { return outer ? outer->post(r) : inner->post(r); }
  };
  auto t = std::make_unique<Owning>();
  if (const char* replay = std::getenv("KARKAS_LLM_REPLAY"); replay != nullptr && *replay != '\0') {
    spdlog::info("llm: replaying {}", replay);
    t->inner = std::make_unique<ReplayTransport>(replay, false);
    return t;
  }
  t->inner = std::make_unique<CurlTransport>();
  if (const char* rec = std::getenv("KARKAS_LLM_RECORD"); rec != nullptr && *rec != '\0') {
    spdlog::info("llm: recording to {}", rec);
    t->outer = std::make_unique<RecordingTransport>(*t->inner, rec);
  }
  return t;
}

}  // namespace kk::llm
