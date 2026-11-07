# Shared test support: doctest main, fixture paths, temporary directories.
add_library(kk_test_support STATIC main.cpp support.cpp)
target_include_directories(kk_test_support PUBLIC include)
target_link_libraries(kk_test_support PUBLIC doctest::doctest)
target_compile_features(kk_test_support PUBLIC cxx_std_23)
target_compile_definitions(kk_test_support PUBLIC
  KK_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  KK_TEST_TMP="${PROJECT_BINARY_DIR}/test_tmp"
  KK_DOCTEST_INCLUDE_DIR="${KK_DOCTEST_INCLUDE_DIR}")
