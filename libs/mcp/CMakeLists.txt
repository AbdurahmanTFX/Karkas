kk_add_library(mcp
  SOURCES src/server.cpp
  PUBLIC_DEPS kk_core kk_index
  PRIVATE_DEPS nlohmann_json::nlohmann_json spdlog::spdlog)

kk_add_tests(mcp SOURCES tests/mcp_test.cpp DEPS nlohmann_json::nlohmann_json)
