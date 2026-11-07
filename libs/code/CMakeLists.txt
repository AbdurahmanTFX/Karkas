kk_add_library(code
  SOURCES src/analyzer.cpp src/drift.cpp src/gen.cpp src/import.cpp
  PUBLIC_DEPS kk_core kk_index
  PRIVATE_DEPS kk_ts_cpp kk_templates nlohmann_json::nlohmann_json)

kk_add_tests(code SOURCES tests/analyzer_test.cpp tests/drift_test.cpp tests/gen_test.cpp tests/imported_test.cpp tests/import_test.cpp DEPS kk_runner)
