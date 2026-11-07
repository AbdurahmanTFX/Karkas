// Contract tests of dsp.chain with the generated mocks of its callees.
#include <doctest/doctest.h>

#include <vector>

#include "chain.hpp"
#include "dsp_echo_mock.hpp"
#include "dsp_gain_mock.hpp"
#include "dsp_lowpass_mock.hpp"

TEST_CASE("output length equals input length") {
  kkmock::MockIGain g;
  kkmock::MockILowpass l;
  kkmock::MockIEcho e;
  auto chain = wk::make_chain(&g, &l, &e);
  for (std::size_t n : {0u, 1u, 7u, 512u}) {
    std::vector<wk::Frame> f(n);
    chain->process(f);
    CHECK(f.size() == n);
  }
  CHECK(g.calls.size() == 4);
  CHECK(e.calls.size() == 4);
}

TEST_CASE("an empty chain passes frames through unchanged") {
  auto chain = wk::make_chain(nullptr, nullptr, nullptr);
  std::vector<wk::Frame> f = {{0.1f, 0.2f}, {-0.3f, 0.4f}};
  auto copy = f;
  chain->process(f);
  CHECK(f[0].left == copy[0].left);
  CHECK(f[1].right == copy[1].right);
}
