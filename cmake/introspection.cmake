# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(CheckCXXSourceCompiles)
include(CheckCXXSymbolExists)
include(CheckIncludeFileCXX)

# The following HAVE_{HEADER}_H variables go to the bitcoin-config.h header.
check_include_file_cxx(sys/prctl.h HAVE_SYS_PRCTL_H)
check_include_file_cxx(sys/resources.h HAVE_SYS_RESOURCES_H)
check_include_file_cxx(sys/vmmeter.h HAVE_SYS_VMMETER_H)
check_include_file_cxx(vm/vm_param.h HAVE_VM_VM_PARAM_H)

check_cxx_symbol_exists(O_CLOEXEC "fcntl.h" HAVE_O_CLOEXEC)
check_cxx_symbol_exists(fdatasync "unistd.h" HAVE_FDATASYNC)
check_cxx_symbol_exists(fork "unistd.h" HAVE_DECL_FORK)
check_cxx_symbol_exists(pipe2 "unistd.h" HAVE_DECL_PIPE2)
check_cxx_symbol_exists(setsid "unistd.h" HAVE_DECL_SETSID)

check_include_file_cxx(sys/types.h HAVE_SYS_TYPES_H)
check_include_file_cxx(ifaddrs.h HAVE_IFADDRS_H)
if(HAVE_SYS_TYPES_H AND HAVE_IFADDRS_H)
  include(TestAppendRequiredLibraries)
  test_append_socket_library(core_interface)
endif()

include(TestAppendRequiredLibraries)
test_append_atomic_library(core_interface)

check_cxx_symbol_exists(std::system "cstdlib" HAVE_STD_SYSTEM)
check_cxx_symbol_exists(::_wsystem "stdlib.h" HAVE__WSYSTEM)
if(HAVE_STD_SYSTEM OR HAVE__WSYSTEM)
  set(HAVE_SYSTEM 1)
endif()

check_cxx_source_compiles("
  #include <string.h>

  int main()
  {
    char buf[100];
    char* p{strerror_r(0, buf, sizeof buf)};
    (void)p;
  }
  " STRERROR_R_CHAR_P
)

# Check for malloc_info (for memory statistics information in getmemoryinfo).
check_cxx_symbol_exists(malloc_info "malloc.h" HAVE_MALLOC_INFO)

# Check for mallopt(M_ARENA_MAX) (to set glibc arenas).
check_cxx_source_compiles("
  #include <malloc.h>

  int main()
  {
    mallopt(M_ARENA_MAX, 1);
  }
  " HAVE_MALLOPT_ARENA_MAX
)

# Check for posix_fallocate().
check_cxx_source_compiles("
  // same as in src/util/fs_helpers.cpp
  #ifdef __linux__
  #ifdef _POSIX_C_SOURCE
  #undef _POSIX_C_SOURCE
  #endif
  #define _POSIX_C_SOURCE 200112L
  #endif // __linux__
  #include <fcntl.h>

  int main()
  {
    return posix_fallocate(0, 0, 0);
  }
  " HAVE_POSIX_FALLOCATE
)

# Check for strong getauxval() support in the system headers.
check_cxx_source_compiles("
  #include <sys/auxv.h>

  int main()
  {
    getauxval(AT_HWCAP);
  }
  " HAVE_STRONG_GETAUXVAL
)

# Check for UNIX sockets.
check_cxx_source_compiles("
  #include <sys/socket.h>
  #include <sys/un.h>

  int main()
  {
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
  }
  " HAVE_SOCKADDR_UN
)

# Check for different ways of gathering OS randomness:
# - Linux getrandom syscall
# NOTE: random.cpp issues the syscall directly rather than calling the libc
#       wrapper, so probe for the syscall and keep the Autotools macro name.
check_cxx_source_compiles("
  #include <unistd.h>
  #include <sys/syscall.h>
  #include <linux/random.h>

  int main()
  {
    syscall(SYS_getrandom, nullptr, 32, 0);
  }
  " HAVE_SYS_GETRANDOM
)

# - BSD getentropy()
check_cxx_source_compiles("
  #include <sys/random.h>

  int main()
  {
    getentropy(nullptr, 32);
  }
  " HAVE_GETENTROPY_RAND
)


# - BSD sysctl()
check_cxx_source_compiles("
  #include <sys/types.h>
  #include <sys/sysctl.h>

  #ifdef __linux__
  #error Don't use sysctl on Linux, it's deprecated even when it works
  #endif

  int main()
  {
    sysctl(nullptr, 2, nullptr, nullptr, nullptr, 0);
  }
  " HAVE_SYSCTL
)

# - BSD sysctl(KERN_ARND)
check_cxx_source_compiles("
  #include <sys/types.h>
  #include <sys/sysctl.h>

  #ifdef __linux__
  #error Don't use sysctl on Linux, it's deprecated even when it works
  #endif

  int main()
  {
    static int name[2] = {CTL_KERN, KERN_ARND};
    sysctl(name, 2, nullptr, nullptr, nullptr, 0);
  }
  " HAVE_SYSCTL_ARND
)

if(NOT MSVC)
  include(CheckSourceCompilesAndLinks)

  # Check for SSSE3 intrinsics, used by the X11 hashing backends.
  set(SSSE3_CXXFLAGS -mssse3)
  check_cxx_source_compiles_with_flags("${SSSE3_CXXFLAGS}" "
    #include <tmmintrin.h>

    int main()
    {
      __m64 x = _mm_abs_pi32(_m_from_int(0));
      return 0;
    }
    " HAVE_SSSE3
  )
  set(ENABLE_SSSE3 ${HAVE_SSSE3})

  # Check for SSE4.1 intrinsics.
  set(SSE41_CXXFLAGS -msse4.1)
  check_cxx_source_compiles_with_flags("${SSE41_CXXFLAGS}" "
    #include <immintrin.h>

    int main()
    {
      __m128i a = _mm_set1_epi32(0);
      __m128i b = _mm_set1_epi32(1);
      __m128i r = _mm_blend_epi16(a, b, 0xFF);
      return _mm_extract_epi32(r, 3);
    }
    " HAVE_SSE41
  )
  set(ENABLE_SSE41 ${HAVE_SSE41})

  # Check for AVX2 intrinsics.
  set(AVX2_CXXFLAGS -mavx -mavx2)
  check_cxx_source_compiles_with_flags("${AVX2_CXXFLAGS}" "
    #include <immintrin.h>

    int main()
    {
      __m256i l = _mm256_set1_epi32(0);
      return _mm256_extract_epi32(l, 7);
    }
    " HAVE_AVX2
  )
  set(ENABLE_AVX2 ${HAVE_AVX2})

  # Check for x86 SHA-NI intrinsics.
  set(X86_SHANI_CXXFLAGS -msse4 -msha)
  check_cxx_source_compiles_with_flags("${X86_SHANI_CXXFLAGS}" "
    #include <immintrin.h>

    int main()
    {
      __m128i i = _mm_set1_epi32(0);
      __m128i j = _mm_set1_epi32(1);
      __m128i k = _mm_set1_epi32(2);
      return _mm_extract_epi32(_mm_sha256rnds2_epu32(i, j, k), 0);
    }
    " HAVE_X86_SHANI
  )
  set(ENABLE_X86_SHANI ${HAVE_X86_SHANI})

  # Check for x86 AES-NI intrinsics, used by the X11 hashing backends.
  set(X86_AESNI_CXXFLAGS -msse4.1 -maes)
  check_cxx_source_compiles_with_flags("${X86_AESNI_CXXFLAGS}" "
    #include <cstdint>
    #include <immintrin.h>
    #include <wmmintrin.h>

    int main()
    {
      __m128i x = _mm_setzero_si128();
      x = _mm_aesenc_si128(x, _mm_setzero_si128());
      return _mm_extract_epi32(x, 0);
    }
    " HAVE_X86_AESNI
  )
  set(ENABLE_X86_AESNI ${HAVE_X86_AESNI})

  # Check for ARMv8 AES intrinsics, used by the X11 hashing backends.
  set(ARM_AES_CXXFLAGS -march=armv8-a+crypto)
  check_cxx_source_compiles_with_flags("${ARM_AES_CXXFLAGS}" "
    #include <arm_neon.h>

    int main()
    {
      uint8x16_t a, b;
      vaesmcq_u8(vaeseq_u8(a, b));
      return 0;
    }
    " HAVE_ARM_AES
  )
  set(ENABLE_ARM_AES ${HAVE_ARM_AES})

  # Check for ARM NEON intrinsics, used by the X11 hashing backends.
  # Unlike the checks above, the required flag differs between ARMv8 and ARMv7.
  # Each attempt needs its own result variable because check_cxx_source_compiles()
  # is a no-op once the result variable is cached, including when cached as false.
  set(neon_source "
    #include <arm_neon.h>

    int main()
    {
      float32x4_t f = vdupq_n_f32(0.0);
      return 0;
    }
  ")
  check_cxx_source_compiles_with_flags("-march=armv8-a" "${neon_source}" HAVE_ARM_NEON_ARMV8)
  if(HAVE_ARM_NEON_ARMV8)
    set(ARM_NEON_CXXFLAGS -march=armv8-a)
    set(HAVE_ARM_NEON TRUE)
  else()
    check_cxx_source_compiles_with_flags("-march=armv7-a;-mfpu=neon" "${neon_source}" HAVE_ARM_NEON_ARMV7)
    if(HAVE_ARM_NEON_ARMV7)
      set(ARM_NEON_CXXFLAGS -march=armv7-a -mfpu=neon)
      set(HAVE_ARM_NEON TRUE)
    endif()
  endif()
  unset(neon_source)
  set(ENABLE_ARM_NEON ${HAVE_ARM_NEON})

  # Check for ARMv8 SHA-NI intrinsics.
  set(ARM_SHANI_CXXFLAGS -march=armv8-a+crypto)
  check_cxx_source_compiles_with_flags("${ARM_SHANI_CXXFLAGS}" "
    #include <arm_neon.h>

    int main()
    {
      uint32x4_t a, b, c;
      vsha256h2q_u32(a, b, c);
      vsha256hq_u32(a, b, c);
      vsha256su0q_u32(a, b);
      vsha256su1q_u32(a, b, c);
    }
    " HAVE_ARM_SHANI
  )
  set(ENABLE_ARM_SHANI ${HAVE_ARM_SHANI})
endif()
