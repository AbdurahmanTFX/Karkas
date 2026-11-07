// Contract tests of stream.pump, with the generated mock of io.reader.
#include <doctest/doctest.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "io_reader_mock.hpp"
#include "pump.hpp"

namespace {

struct Sink final : wk::IFrameSink {
  std::mutex mu;
  std::vector<wk::Frame> got;
  bool push(std::span<const wk::Frame> f) override {
    std::lock_guard lk(mu);
    got.insert(got.end(), f.begin(), f.end());
    return true;
  }
};

}  // namespace

TEST_CASE("stops cleanly on end of file and on stop()") {
  kkmock::MockIReader reader;
  int left = 1000;
  reader.on_frames = [&](std::span<wk::Frame> out) {
    std::size_t n = std::min<std::size_t>(out.size(), static_cast<std::size_t>(left));
    for (std::size_t i = 0; i < n; ++i) out[i] = wk::Frame{static_cast<float>(left - static_cast<int>(i)), 0.0f};
    left -= static_cast<int>(n);
    return n;
  };
  Sink sink;
  auto pump = wk::make_pump(reader, sink, 64);
  pump->start();
  for (int i = 0; i < 200 && !pump->finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(pump->finished());
  pump->stop();
  CHECK(sink.got.size() == 1000);

  // stop() while the source never ends.
  kkmock::MockIReader endless;
  endless.on_frames = [](std::span<wk::Frame> out) { return out.size(); };
  Sink sink2;
  auto p2 = wk::make_pump(endless, sink2, 16);
  p2->start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  p2->stop();
  CHECK_FALSE(p2->finished());
}
