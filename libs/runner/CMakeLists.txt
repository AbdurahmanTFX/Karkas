kk_add_library(runner
  SOURCES src/runner.cpp
  PUBLIC_DEPS kk_core
  PRIVATE_DEPS reproc++)

kk_add_tests(runner SOURCES tests/runner_test.cpp)
