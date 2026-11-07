kk_add_library(layout
  SOURCES src/sugiyama.cpp src/graphviz.cpp src/layout.cpp
  PUBLIC_DEPS kk_core kk_index
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
if(GRAPHVIZ_FOUND)
  target_link_libraries(kk_layout PRIVATE PkgConfig::GRAPHVIZ)
  target_compile_definitions(kk_layout PRIVATE KK_HAVE_GRAPHVIZ=1)
endif()

kk_add_tests(layout SOURCES tests/layout_test.cpp)
