# kk_set_warnings(<target>) — project-wide warning policy (19.5).
# -Werror is enabled only in CI via -DKARKAS_WERROR=ON.
function(kk_set_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-missing-field-initializers
      $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Wold-style-cast>)
    if(KARKAS_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
