kk_add_library(index
  SOURCES src/db.cpp src/hash.cpp src/state.cpp src/index.cpp src/views.cpp src/watch.cpp
  PUBLIC_DEPS kk_core
  PRIVATE_DEPS ${KK_SQLITE_TARGET} kk_xxhash nlohmann_json::nlohmann_json spdlog::spdlog)

kk_add_tests(index
  SOURCES tests/index_test.cpp tests/views_test.cpp tests/perf_test.cpp)
