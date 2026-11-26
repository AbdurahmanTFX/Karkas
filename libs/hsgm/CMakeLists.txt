kk_add_library(hsgm
  SOURCES src/input.cpp src/diag.cpp
  PUBLIC_DEPS kk_core kk_index)

kk_add_tests(hsgm SOURCES tests/support.cpp tests/diag_test.cpp)
