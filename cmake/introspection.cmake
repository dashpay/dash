# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

include(CheckCXXSourceCompiles)
include(CheckCXXSymbolExists)
include(CheckIncludeFileCXX)
include(TestBigEndian)

test_big_endian(WORDS_BIGENDIAN)

CHECK_INCLUDE_FILE_CXX(byteswap.h HAVE_BYTESWAP_H)
CHECK_INCLUDE_FILE_CXX(endian.h HAVE_ENDIAN_H)
CHECK_INCLUDE_FILE_CXX(stdio.h HAVE_STDIO_H)
CHECK_INCLUDE_FILE_CXX(stdlib.h HAVE_STDLIB_H)
CHECK_INCLUDE_FILE_CXX(strings.h HAVE_STRINGS_H)
CHECK_INCLUDE_FILE_CXX(sys/endian.h HAVE_SYS_ENDIAN_H)
CHECK_INCLUDE_FILE_CXX(sys/prctl.h HAVE_SYS_PRCTL_H)
CHECK_INCLUDE_FILE_CXX(sys/resources.h HAVE_SYS_RESOURCES_H)
CHECK_INCLUDE_FILE_CXX(sys/select.h HAVE_SYS_SELECT_H)
CHECK_INCLUDE_FILE_CXX(sys/stat.h HAVE_SYS_STAT_H)
CHECK_INCLUDE_FILE_CXX(sys/sysctl.h HAVE_SYS_SYSCTL_H)
CHECK_INCLUDE_FILE_CXX(sys/types.h HAVE_SYS_TYPES_H)
CHECK_INCLUDE_FILE_CXX(sys/vmmeter.h HAVE_SYS_VMMETER_H)
CHECK_INCLUDE_FILE_CXX(unistd.h HAVE_UNISTD_H)
CHECK_INCLUDE_FILE_CXX(vm/vm_param.h HAVE_VM_VM_PARAM_H)

if(HAVE_BYTESWAP_H)
  check_cxx_symbol_exists(bswap_16 "byteswap.h" HAVE_DECL_BSWAP_16)
  check_cxx_symbol_exists(bswap_32 "byteswap.h" HAVE_DECL_BSWAP_32)
  check_cxx_symbol_exists(bswap_64 "byteswap.h" HAVE_DECL_BSWAP_64)
endif()

if(HAVE_ENDIAN_H OR HAVE_SYS_ENDIAN_H)
  if(HAVE_ENDIAN_H)
    set(ENDIAN_HEADER "endian.h")
  else()
    set(ENDIAN_HEADER "sys/endian.h")
  endif()
  check_cxx_symbol_exists(be16toh ${ENDIAN_HEADER} HAVE_DECL_BE16TOH)
  check_cxx_symbol_exists(be32toh ${ENDIAN_HEADER} HAVE_DECL_BE32TOH)
  check_cxx_symbol_exists(be64toh ${ENDIAN_HEADER} HAVE_DECL_BE64TOH)
  check_cxx_symbol_exists(htobe16 ${ENDIAN_HEADER} HAVE_DECL_HTOBE16)
  check_cxx_symbol_exists(htobe32 ${ENDIAN_HEADER} HAVE_DECL_HTOBE32)
  check_cxx_symbol_exists(htobe64 ${ENDIAN_HEADER} HAVE_DECL_HTOBE64)
  check_cxx_symbol_exists(htole16 ${ENDIAN_HEADER} HAVE_DECL_HTOLE16)
  check_cxx_symbol_exists(htole32 ${ENDIAN_HEADER} HAVE_DECL_HTOLE32)
  check_cxx_symbol_exists(htole64 ${ENDIAN_HEADER} HAVE_DECL_HTOLE64)
  check_cxx_symbol_exists(le16toh ${ENDIAN_HEADER} HAVE_DECL_LE16TOH)
  check_cxx_symbol_exists(le32toh ${ENDIAN_HEADER} HAVE_DECL_LE32TOH)
  check_cxx_symbol_exists(le64toh ${ENDIAN_HEADER} HAVE_DECL_LE64TOH)
endif()

if(HAVE_UNISTD_H)
  check_cxx_symbol_exists(fdatasync "unistd.h" HAVE_FDATASYNC)
  check_cxx_symbol_exists(fork "unistd.h" HAVE_DECL_FORK)
  check_cxx_symbol_exists(pipe2 "unistd.h" HAVE_DECL_PIPE2)
  check_cxx_symbol_exists(setsid "unistd.h" HAVE_DECL_SETSID)
endif()

# Check for gmtime_r(), fallback to gmtime_s() if that is unavailable.
# Fail if neither are available.
check_cxx_source_compiles("
#include <ctime>

int main()
{
  gmtime_r((const time_t*)nullptr, (struct tm*)nullptr);
}
" HAVE_GMTIME_R)
if(NOT HAVE_GMTIME_R)
  check_cxx_source_compiles("
  #include <ctime>

  int main()
  {
    gmtime_s((struct tm*)nullptr, (const time_t*)nullptr);
  }
  " HAVE_GMTIME_S)
  if(NOT HAVE_GMTIME_S)
    message(FATAL_ERROR "Both gmtime_r and gmtime_s are unavailable")
  endif()
endif()

if(WIN32)
  check_cxx_source_compiles("
  #include <stdlib.h>

  int main()
  {
    return ::_wsystem(NULL);
  }
  " HAVE_SYSTEM)
else()
  check_cxx_source_compiles("
  #include <cstdlib>

  int main()
  {
    return std::system(nullptr);
  }
  " HAVE_SYSTEM)
endif()
