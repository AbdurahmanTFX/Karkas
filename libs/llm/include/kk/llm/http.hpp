#pragma once
// HTTP transport and record/replay (16.4). Providers talk to an IHttpTransport, so
// tests replay recorded exchanges without network or money.
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kk/core/error.hpp"

namespace kk::llm {

struct HttpRequest {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  int timeout_s = 300;
};

struct HttpResponse {
  int status = 0;
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
};

class IHttpTransport {
 public:
  virtual ~IHttpTransport() = default;
  // Returns an error only for transport failures (DNS, TLS, timeout); HTTP error
  // statuses come back as responses.
  virtual kk::Result<HttpResponse> post(const HttpRequest& req) = 0;
};

// libcurl, one handle per thread with keep-alive (9.8).
class CurlTransport final : public IHttpTransport {
 public:
  CurlTransport();
  kk::Result<HttpResponse> post(const HttpRequest& req) override;
};

// Appends every exchange to a JSONL cassette. Secret headers (x-api-key,
// authorization) are never written.
class RecordingTransport final : public IHttpTransport {
 public:
  RecordingTransport(IHttpTransport& inner, std::filesystem::path cassette);
  kk::Result<HttpResponse> post(const HttpRequest& req) override;

 private:
  IHttpTransport& inner_;
  std::filesystem::path cassette_;
  std::mutex mu_;
};

// Plays a cassette back in order. With `strict`, each request body must equal the
// recorded one (ignoring JSON key order and whitespace).
class ReplayTransport final : public IHttpTransport {
 public:
  explicit ReplayTransport(std::filesystem::path cassette, bool strict = false);
  kk::Result<HttpResponse> post(const HttpRequest& req) override;
  [[nodiscard]] std::size_t remaining() const;
  [[nodiscard]] const std::vector<std::string>& requests() const { return seen_; }  // bodies sent

 private:
  struct Entry {
    std::string url, body;
    HttpResponse response;
    std::string transport_error;
  };
  std::vector<Entry> entries_;
  std::size_t next_ = 0;
  bool strict_ = false;
  std::string load_error_;
  std::vector<std::string> seen_;
  mutable std::mutex mu_;
};

// Global transport selection: KARKAS_LLM_REPLAY=<cassette> or KARKAS_LLM_RECORD=<cassette>
// wrap the curl transport (used by the CLI and the bench harness).
std::unique_ptr<IHttpTransport> make_transport_from_env();

}  // namespace kk::llm
