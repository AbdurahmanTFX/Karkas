#pragma once
// kk::Error and kk::Result<T> — errors never cross module boundaries as exceptions (6.6).
#include <expected>
#include <string>
#include <utility>

namespace kk {

struct SourceLoc {
  std::string file;
  int line = 0;  // 1-based; 0 = unknown
  int col = 0;   // 1-based; 0 = unknown
};

struct Error {
  std::string code;     // "S03", "E01", "io", "sqlite", "http", ...
  std::string message;  // short English text
  SourceLoc loc;

  // "file:line:col: error S03: message" (parts omitted when unknown).
  [[nodiscard]] std::string str() const;
};

template <class T>
using Result = std::expected<T, Error>;
using VoidResult = std::expected<void, Error>;

inline std::unexpected<Error> fail(std::string code, std::string message, SourceLoc loc = {}) {
  return std::unexpected<Error>(Error{std::move(code), std::move(message), std::move(loc)});
}

}  // namespace kk
