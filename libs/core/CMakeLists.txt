kk_add_library(core
  SOURCES src/text.cpp src/ksl.cpp src/model.cpp src/schema.cpp src/validate.cpp
          src/paths.cpp src/config.cpp src/tickets.cpp src/cancel.cpp src/notes.cpp src/markdown.cpp
  PUBLIC_DEPS fmt::fmt
  PRIVATE_DEPS tomlplusplus::tomlplusplus)

kk_add_tests(core
  SOURCES tests/ksl_test.cpp tests/model_test.cpp tests/roundtrip_test.cpp
          tests/validate_test.cpp tests/util_test.cpp tests/tickets_test.cpp tests/notes_test.cpp
          tests/markdown_test.cpp)
