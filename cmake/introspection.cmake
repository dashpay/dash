# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

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
