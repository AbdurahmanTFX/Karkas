kk_add_library(hsgm
  SOURCES src/input.cpp src/text.cpp src/graph.cpp src/diag.cpp src/views.cpp src/propagation.cpp
  PUBLIC_DEPS kk_core kk_index)

kk_add_tests(hsgm SOURCES tests/support.cpp tests/diag_test.cpp tests/views_test.cpp tests/propagation_test.cpp)
