# Helpers for Karkas libraries: kk_add_library(<name> ...) and kk_add_tests(<name> ...).
#
#   kk_add_library(core SOURCES src/a.cpp ... PUBLIC_DEPS ... PRIVATE_DEPS ...)
#     -> target kk_core, alias karkas::core, include/ is PUBLIC, C++23, warnings.
#   kk_add_tests(core SOURCES tests/a.cpp ... DEPS ... SLOW "<test case name>" ... SERIAL "<test case name>" ...)
#     -> executable kk_core_tests, doctest discovery, ctest label "core"; the SLOW test cases (real
#        builds of reference projects, minutes each) also get the label "slow":
#        `ctest --preset dev-fast` (or -LE slow) leaves them out, `ctest --preset dev -L slow` runs them.

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
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;SLOW;SERIAL" ${ARGN})
  add_executable(kk_${name}_tests ${ARG_SOURCES})
  target_link_libraries(kk_${name}_tests PRIVATE kk_${name} kk_test_support ${ARG_DEPS})
  kk_set_warnings(kk_${name}_tests)
  # A test that ran no case failed to run (a ';' in a TEST_CASE name cuts the name CTest passes
  # back, and such tests "passed" with 0 cases): FAIL_REGULAR_EXPRESSION makes it visible.
  doctest_discover_tests(kk_${name}_tests
    TEST_PREFIX "${name}/"
    PROPERTIES LABELS ${name} FAIL_REGULAR_EXPRESSION "test cases: *0 *[|]"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
  # SERIAL: timing tests (a perf budget in ms) run alone; next to parallel builds they measure the load.
  if(ARG_SERIAL)
    set(serial_content "")
    foreach(test_case IN LISTS ARG_SERIAL)
      string(APPEND serial_content "set_tests_properties([==[${name}/${test_case}]==] PROPERTIES RUN_SERIAL TRUE)\n")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kk_${name}_serial.cmake" "${serial_content}")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${CMAKE_CURRENT_BINARY_DIR}/kk_${name}_serial.cmake")
  endif()
  # The discovered tests come from an include file CTest reads at run time; a second include file,
  # read after it, adds the label "slow" to the named cases (ADR 0029). Their reference builds get
  # ccache when it is there (CMake takes CMAKE_<LANG>_COMPILER_LAUNCHER from the environment; the
  # base dir and no cwd hash let the unique temp folders of the tests share the cache).
  if(ARG_SLOW)
    set(slow_file "${CMAKE_CURRENT_BINARY_DIR}/kk_${name}_slow_labels.cmake")
    set(env "")
    if(KK_CCACHE)
      set(env " ENVIRONMENT \"CMAKE_CXX_COMPILER_LAUNCHER=${KK_CCACHE};CMAKE_C_COMPILER_LAUNCHER=${KK_CCACHE};CCACHE_BASEDIR=${CMAKE_BINARY_DIR}/test_tmp;CCACHE_NOHASHDIR=1\"")
    endif()
    set(content "")
    foreach(test_case IN LISTS ARG_SLOW)
      string(APPEND content "set_tests_properties([==[${name}/${test_case}]==] PROPERTIES LABELS \"${name};slow\"${env})\n")
    endforeach()
    file(WRITE "${slow_file}" "${content}")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${slow_file}")
  endif()
endfunction()
