#include "pump.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "karkas_probe.hpp"
#include "reader.hpp"

namespace wk {

namespace {

class Pump final : public IPump {
 public:
  Pump(IReader& source, IFrameSink& sink, std::size_t chunk) : source_(source), sink_(sink), chunk_(chunk) {}
  ~Pump() override { stop(); }

  void start() override {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
  }

  void stop() override {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  [[nodiscard]] bool finished() const override { return finished_; }

 private:
  void loop() {
    std::vector<Frame> buf(chunk_);
    while (!stop_) {
      std::size_t n = source_.frames(buf);
      if (n == 0) break;
      std::span<const Frame> part(buf.data(), n);
      while (!stop_ && !sink_.push(part)) std::this_thread::sleep_for(std::chrono::microseconds(200));
      KK_PROBE("io.reader->stream.pump", "chunk", n);
    }
    finished_ = !stop_ || finished_;
  }

  IReader& source_;
  IFrameSink& sink_;
  std::size_t chunk_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> finished_{false};
};

}  // namespace

std::unique_ptr<IPump> make_pump(IReader& source, IFrameSink& sink, std::size_t chunk) {
  return std::make_unique<Pump>(source, sink, chunk);
}

}  // namespace wk
