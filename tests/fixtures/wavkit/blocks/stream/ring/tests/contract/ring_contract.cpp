// Contract tests of stream.ring: one test per inv, loops over many sizes (tester prompt).
#include <doctest/doctest.h>

#include <thread>
#include <vector>

#include "ring.hpp"

TEST_CASE("never loses or duplicates frames, including at wraparound") {
  for (std::size_t cap : {4u, 8u, 64u}) {
    for (std::size_t chunk = 1; chunk <= cap; ++chunk) {
      auto ring = wk::make_ring(cap);
      float next_in = 0.0f;
      float next_out = 0.0f;
      std::vector<wk::Frame> in(chunk);
      std::vector<wk::Frame> out(cap);
      for (int round = 0; round < 50; ++round) {
        for (auto& f : in) f = wk::Frame{next_in++, 0.0f};
        REQUIRE(ring->push(in));
        std::size_t n = ring->pop(out);
        REQUIRE(n == chunk);
        for (std::size_t i = 0; i < n; ++i) CHECK(out[i].left == next_out++);
      }
    }
  }
  // Producer and consumer threads.
  auto ring = wk::make_ring(256);
  const int total = 100000;
  std::thread producer([&] {
    std::vector<wk::Frame> one(7);
    int sent = 0;
    while (sent < total) {
      std::size_t n = std::min<std::size_t>(7, static_cast<std::size_t>(total - sent));
      for (std::size_t i = 0; i < n; ++i) one[i] = wk::Frame{static_cast<float>(sent + static_cast<int>(i)), 0.0f};
      if (ring->push(std::span<const wk::Frame>(one.data(), n))) sent += static_cast<int>(n);
    }
  });
  int got = 0;
  bool ordered = true;
  std::vector<wk::Frame> buf(32);
  while (got < total) {
    std::size_t n = ring->pop(buf);
    for (std::size_t i = 0; i < n; ++i) ordered = ordered && buf[i].left == static_cast<float>(got++);
  }
  producer.join();
  CHECK(ordered);
  CHECK(got == total);
}

TEST_CASE("push and pop never block") {
  auto ring = wk::make_ring(8);
  std::vector<wk::Frame> nine(9);
  CHECK_FALSE(ring->push(nine));
  std::vector<wk::Frame> out(4);
  CHECK(ring->pop(out) == 0);
  std::vector<wk::Frame> eight(8);
  CHECK(ring->push(eight));
  CHECK_FALSE(ring->push(std::span<const wk::Frame>(eight.data(), 1)));
}

TEST_CASE("fill() is always within [0, capacity()]") {
  auto ring = wk::make_ring(16);
  CHECK(ring->capacity() == 16);
  std::vector<wk::Frame> f(5);
  std::vector<wk::Frame> out(3);
  for (int i = 0; i < 100; ++i) {
    (void)ring->push(f);
    CHECK(ring->fill() <= ring->capacity());
    (void)ring->pop(out);
    CHECK(ring->fill() <= ring->capacity());
  }
}
