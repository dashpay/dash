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

add_library(bitcoin_crypto_sph INTERFACE)
target_compile_definitions(bitcoin_crypto_sph INTERFACE
  SPH_SMALL_FOOTPRINT_CUBEHASH=1
  SPH_SMALL_FOOTPRINT_JH=1
)
target_link_libraries(bitcoin_crypto PRIVATE bitcoin_crypto_sph)
