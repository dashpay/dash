# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(bitcoin_crypto STATIC EXCLUDE_FROM_ALL
  aes.cpp
  chacha20.cpp
  chacha20poly1305.cpp
  hkdf_sha256_32.cpp
  hmac_sha256.cpp
  hmac_sha512.cpp
  muhash.cpp
  pkcs5_pbkdf2_hmac_sha512.cpp
  poly1305.cpp
  ripemd160.cpp
  sha1.cpp
  sha256.cpp
  sha256_sse4.cpp
  sha3.cpp
  sha512.cpp
  siphash.cpp
  x11/aes.cpp
  x11/blake.c
  x11/bmw.c
  x11/cubehash.c
  x11/dispatch.cpp
  x11/echo.cpp
  x11/groestl.c
  x11/jh.c
  x11/keccak.c
  x11/luffa.c
  x11/shavite.cpp
  x11/simd.c
  x11/skein.c
  ../support/cleanse.cpp
)

target_link_libraries(bitcoin_crypto
  PRIVATE
    core_interface
)

if(HAVE_SSE41)
  add_library(bitcoin_crypto_sse41 STATIC EXCLUDE_FROM_ALL
    sha256_sse41.cpp
  )
  target_compile_definitions(bitcoin_crypto_sse41 PUBLIC ENABLE_SSE41)
  target_compile_options(bitcoin_crypto_sse41 PRIVATE ${SSE41_CXXFLAGS})
  target_link_libraries(bitcoin_crypto_sse41 PRIVATE core_interface)
  target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_sse41)
endif()

if(HAVE_AVX2)
  add_library(bitcoin_crypto_avx2 STATIC EXCLUDE_FROM_ALL
    sha256_avx2.cpp
  )
  target_compile_definitions(bitcoin_crypto_avx2 PUBLIC ENABLE_AVX2)
  target_compile_options(bitcoin_crypto_avx2 PRIVATE ${AVX2_CXXFLAGS})
  target_link_libraries(bitcoin_crypto_avx2 PRIVATE core_interface)
  target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_avx2)
endif()

if(HAVE_SSE41 AND HAVE_X86_SHANI)
  add_library(bitcoin_crypto_x86_shani STATIC EXCLUDE_FROM_ALL
    sha256_x86_shani.cpp
  )
  target_compile_definitions(bitcoin_crypto_x86_shani PUBLIC ENABLE_SSE41 ENABLE_X86_SHANI)
  target_compile_options(bitcoin_crypto_x86_shani PRIVATE ${X86_SHANI_CXXFLAGS})
  target_link_libraries(bitcoin_crypto_x86_shani PRIVATE core_interface)
  target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_x86_shani)
endif()

if(HAVE_ARM_SHANI)
  add_library(bitcoin_crypto_arm_shani STATIC EXCLUDE_FROM_ALL
    sha256_arm_shani.cpp
  )
  target_compile_definitions(bitcoin_crypto_arm_shani PUBLIC ENABLE_ARM_SHANI)
  target_compile_options(bitcoin_crypto_arm_shani PRIVATE ${ARM_SHANI_CXXFLAGS})
  target_link_libraries(bitcoin_crypto_arm_shani PRIVATE core_interface)
  target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_arm_shani)
endif()

# X11 hashing backends. The generic implementations in x11/ are always built;
# these libraries provide the SIMD variants that x11/dispatch.cpp selects at
# runtime. Keep the definitions in sync with cmake/bitcoin-config.h.in, which
# is what dispatch.cpp checks when deciding whether a backend exists.
if(HAVE_SSSE3)
  add_library(bitcoin_crypto_x11_ssse3 STATIC EXCLUDE_FROM_ALL
    x11/ssse3/echo.cpp
  )
  target_compile_definitions(bitcoin_crypto_x11_ssse3 PUBLIC ENABLE_SSSE3)
  target_compile_options(bitcoin_crypto_x11_ssse3 PRIVATE ${SSSE3_CXXFLAGS})
  target_link_libraries(bitcoin_crypto_x11_ssse3 PRIVATE core_interface)
  target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_x11_ssse3)
endif()

if(HAVE_X86_AESNI)
  add_library(bitcoin_crypto_x11_x86_aesni STATIC EXCLUDE_FROM_ALL
    x11/x86_aesni/echo.cpp
    x11/x86_aesni/shavite.cpp
  )
  target_compile_definitions(bitcoin_crypto_x11_x86_aesni PUBLIC ENABLE_SSE41 ENABLE_X86_AESNI)
  target_compile_options(bitcoin_crypto_x11_x86_aesni PRIVATE ${X86_AESNI_CXXFLAGS})
  target_link_libraries(bitcoin_crypto_x11_x86_aesni PRIVATE core_interface)
  target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_x11_x86_aesni)
endif()

if(HAVE_ARM_AES)
  add_library(bitcoin_crypto_x11_arm_aes STATIC EXCLUDE_FROM_ALL
    x11/arm_crypto/echo.cpp
    x11/arm_crypto/shavite.cpp
  )
  target_compile_definitions(bitcoin_crypto_x11_arm_aes PUBLIC ENABLE_ARM_AES)
  target_compile_options(bitcoin_crypto_x11_arm_aes PRIVATE ${ARM_AES_CXXFLAGS})
  target_link_libraries(bitcoin_crypto_x11_arm_aes PRIVATE core_interface)
  target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_x11_arm_aes)
endif()

if(HAVE_ARM_NEON)
  add_library(bitcoin_crypto_x11_arm_neon STATIC EXCLUDE_FROM_ALL
    x11/arm_neon/echo.cpp
  )
  target_compile_definitions(bitcoin_crypto_x11_arm_neon PUBLIC ENABLE_ARM_NEON)
  target_compile_options(bitcoin_crypto_x11_arm_neon PRIVATE ${ARM_NEON_CXXFLAGS})
  target_link_libraries(bitcoin_crypto_x11_arm_neon PRIVATE core_interface)
  target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_x11_arm_neon)
endif()

add_library(bitcoin_crypto_sph INTERFACE)
target_compile_definitions(bitcoin_crypto_sph INTERFACE
  SPH_SMALL_FOOTPRINT_CUBEHASH=1
  SPH_SMALL_FOOTPRINT_JH=1
)
target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_sph)
