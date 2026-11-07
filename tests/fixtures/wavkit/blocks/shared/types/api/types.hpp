#pragma once
// Common data types of wavkit (shared.types).
#include <string>
#include <utility>
#include <variant>

namespace wk {

// One stereo sample pair, normalized to [-1, 1].
struct Frame {
  float left = 0.0f;
  float right = 0.0f;
};

struct AudioFormat {
  int sample_rate = 48000;
  int channels = 2;
  int bits = 16;
  bool is_float = false;
};

struct Unit {};

// Value or error message; errors never cross block boundaries as exceptions.
template <class T>
class Result {
 public:
  Result() : v_(std::in_place_index<1>, "not set") {}
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
  static Result fail(std::string message) {
    Result r;
    r.v_.template emplace<1>(std::move(message));
    return r;
  }
  [[nodiscard]] bool ok() const { return v_.index() == 0; }
  explicit operator bool() const { return ok(); }
  const T& value() const { return std::get<0>(v_); }
  T& value() { return std::get<0>(v_); }
  [[nodiscard]] const std::string& error() const { return std::get<1>(v_); }

 private:
  std::variant<T, std::string> v_;
};

}  // namespace wk
