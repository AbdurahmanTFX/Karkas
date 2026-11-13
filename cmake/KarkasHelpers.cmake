# Helpers for Karkas libraries: kk_add_library(<name> ...) and kk_add_tests(<name> ...).
#
#   kk_add_library(core SOURCES src/a.cpp ... PUBLIC_DEPS ... PRIVATE_DEPS ...)
#     -> target kk_core, alias karkas::core, include/ is PUBLIC, C++23, warnings.
#   kk_add_tests(core SOURCES tests/a.cpp ... DEPS ...)
#     -> executable kk_core_tests, doctest discovery, ctest label "core".

function(kk_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
  add_library(kk_${name} STATIC ${ARG_SOURCES})
  add_library(karkas::${name} ALIAS kk_${name})
  target_include_directories(kk_${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(kk_${name} PUBLIC cxx_std_23)
  target_link_libraries(kk_${name} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
  kk_set_warnings(kk_${name})
endfunction()

function(kk_add_tests name)
  if(NOT KARKAS_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(kk_${name}_tests ${ARG_SOURCES})
  target_link_libraries(kk_${name}_tests PRIVATE kk_${name} kk_test_support ${ARG_DEPS})
  kk_set_warnings(kk_${name}_tests)
  # A test that ran no case failed to run (a ';' in a TEST_CASE name cuts the name CTest passes
  # back, and such tests "passed" with 0 cases): FAIL_REGULAR_EXPRESSION makes it visible.
  doctest_discover_tests(kk_${name}_tests
    TEST_PREFIX "${name}/"
    PROPERTIES LABELS ${name} FAIL_REGULAR_EXPRESSION "test cases: *0 *[|]"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()
