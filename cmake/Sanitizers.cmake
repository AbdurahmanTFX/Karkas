# ASan + UBSan for the whole Karkas build when KARKAS_SANITIZERS=ON (preset "asan").
if(KARKAS_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  else()
    message(WARNING "karkas: sanitizers are only configured for GCC and Clang")
  endif()
endif()
