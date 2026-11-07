kk_add_library(metrics
  SOURCES src/metrics.cpp
  PUBLIC_DEPS kk_core kk_index
  PRIVATE_DEPS nlohmann_json::nlohmann_json)

kk_add_tests(metrics SOURCES tests/metrics_test.cpp)
