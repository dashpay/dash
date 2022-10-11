# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

function(copy_target_for_tests target)
  if(MSVC)
    add_custom_command(
      TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${target}>" "${CMAKE_BINARY_DIR}/src"
      VERBATIM
    )
  endif()
endfunction()
