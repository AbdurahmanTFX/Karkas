#include "ring.hpp"

#include <atomic>
#include <vector>

#include "karkas_probe.hpp"

namespace wk {

namespace {

std::size_t pow2(std::size_t n) {
  std::size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

class Ring final : public IRing {
 public:
  explicit Ring(std::size_t cap) : buf_(pow2(cap)), mask_(buf_.size() - 1) {}

  bool push(std::span<const Frame> frames) override {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (frames.size() > buf_.size() - (head - tail)) return false;
    for (std::size_t i = 0; i < frames.size(); ++i) buf_[(head + i) & mask_] = frames[i];
    head_.store(head + frames.size(), std::memory_order_release);
    KK_PROBE("stream.pump->stream.ring", "fill", head + frames.size() - tail);
    return true;
  }

  std::size_t pop(std::span<Frame> out) override {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t n = std::min(out.size(), head - tail);
    for (std::size_t i = 0; i < n; ++i) out[i] = buf_[(tail + i) & mask_];
    tail_.store(tail + n, std::memory_order_release);
    return n;
  }

  [[nodiscard]] std::size_t capacity() const override { return buf_.size(); }
  [[nodiscard]] std::size_t fill() const override {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }

 private:
  std::vector<Frame> buf_;
  std::size_t mask_;
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

}  // namespace

std::unique_ptr<IRing> make_ring(std::size_t capacity) { return std::make_unique<Ring>(capacity); }

}  // namespace wk
