// Integration test of the dsp group: the chain with the real effects.
#include <doctest/doctest.h>

#include <vector>

#include "chain.hpp"
#include "echo.hpp"
#include "gain.hpp"
#include "lowpass.hpp"

TEST_CASE("dsp: gain, lowpass and echo run in order and keep the length") {
  auto g = wk::make_gain();
  g->set_gain(0.5f);
  auto l = wk::make_lowpass();
  l->set_cutoff(20000.0f, 48000.0f);
  auto e = wk::make_echo();
  e->set_params(10, 0.0f, 0.0f);
  auto chain = wk::make_chain(g.get(), l.get(), e.get());
  std::vector<wk::Frame> f(1000, wk::Frame{0.8f, 0.8f});
  chain->process(f);
  CHECK(f.size() == 1000);
  CHECK(f.back().left == doctest::Approx(0.4f).epsilon(0.01));
}
