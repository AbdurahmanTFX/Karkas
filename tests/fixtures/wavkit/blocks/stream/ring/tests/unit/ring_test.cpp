#include <doctest/doctest.h>

#include "ring.hpp"

TEST_CASE("capacity rounds up to a power of two") {
  CHECK(wk::make_ring(5)->capacity() == 8);
  CHECK(wk::make_ring(16)->capacity() == 16);
}
