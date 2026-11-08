kk_add_library(agents
  SOURCES src/sandbox.cpp src/loop.cpp src/tools.cpp src/router.cpp src/testing.cpp src/changes.cpp src/edit.cpp src/engine.cpp src/bench.cpp
  PUBLIC_DEPS kk_core kk_index kk_code kk_runner kk_llm kk_metrics
  PRIVATE_DEPS kk_templates nlohmann_json::nlohmann_json spdlog::spdlog)
target_compile_definitions(kk_agents PRIVATE KK_DOCTEST_INCLUDE_DIR="${KK_DOCTEST_INCLUDE_DIR}")

kk_add_tests(agents
  SOURCES tests/scripted.cpp tests/router_test.cpp tests/testing_test.cpp tests/sandbox_test.cpp tests/loop_test.cpp tests/engine_test.cpp tests/changes_test.cpp tests/edit_test.cpp tests/bench_test.cpp
  DEPS nlohmann_json::nlohmann_json)
