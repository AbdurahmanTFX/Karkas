kk_add_library(llm
  SOURCES src/http.cpp src/provider.cpp src/anthropic.cpp src/openai.cpp
  PUBLIC_DEPS kk_core
  PRIVATE_DEPS CURL::libcurl nlohmann_json::nlohmann_json spdlog::spdlog)

kk_add_tests(llm SOURCES tests/llm_test.cpp DEPS nlohmann_json::nlohmann_json)
