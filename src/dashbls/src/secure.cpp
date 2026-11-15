// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING.MIT or https://opensource.org/license/MIT

#if defined(__APPLE__)
#define __STDC_WANT_LIB_EXT1__ 1
#endif

#include "secure.h"
#include "util.hpp"

#if BLSALLOC_MIMALLOC
#include "mimalloc.h"
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace bls {
namespace util {
void SecureWipe(void* ptr, size_t len) noexcept
{
    if (ptr == nullptr || len == 0) {
        return;
    }
#if defined(_WIN32)
    SecureZeroMemory(ptr, len);
#elif defined(__APPLE__)
    (void)memset_s(ptr, len, 0, len);
#elif __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25)
    explicit_bzero(ptr, len);
#else
    std::memset(ptr, 0, len);
    // A barrier the compiler may not reorder across, so the memset above cannot
    // be dropped as a dead store on a buffer nothing reads again.
    __asm__ __volatile__("" : : "r"(ptr) : "memory");
#endif
}

namespace {
#if BLSALLOC_MIMALLOC
std::atomic<Util::SecureAllocCallback> g_pfnSecureAlloc{mi_malloc};
std::atomic<Util::SecureFreeCallback> g_pfnSecureFree{mi_free};
#else
std::atomic<Util::SecureAllocCallback> g_pfnSecureAlloc{std::malloc};
std::atomic<Util::SecureFreeCallback> g_pfnSecureFree{std::free};
#endif
// Each pair is installed and read under this, so its halves never mix.
std::mutex g_mutexSecureAllocator;
} // anonymous namespace

void SetSecureAllocator(Util::SecureAllocCallback pfnAlloc,
                        Util::SecureFreeCallback pfnFree)
{
    if (pfnAlloc == nullptr || pfnFree == nullptr) {
        throw std::invalid_argument("secure allocator must not be null");
    }
    std::lock_guard<std::mutex> lock(g_mutexSecureAllocator);
    g_pfnSecureAlloc.store(pfnAlloc);
    g_pfnSecureFree.store(pfnFree);
}

void GetSecureAllocator(Util::SecureAllocCallback* ppfnAlloc,
                        Util::SecureFreeCallback* ppfnFree)
{
    std::lock_guard<std::mutex> lock(g_mutexSecureAllocator);
    if (ppfnAlloc != nullptr) {
        *ppfnAlloc = g_pfnSecureAlloc.load();
    }
    if (ppfnFree != nullptr) {
        *ppfnFree = g_pfnSecureFree.load();
    }
}

void* SecMalloc(size_t nBytes)
{
    if (nBytes == 0) {
        // malloc(0) may return nullptr, round up like `operator new`
        nBytes++;
    }
    void* pAllocation = g_pfnSecureAlloc.load()(nBytes);
    if (pAllocation == nullptr) {
        throw std::bad_alloc();
    }
    return pAllocation;
}

void SecFree(void* ptr, size_t nBytes)
{
    // An installed callback need not accept nullptr as free does.
    if (ptr == nullptr) {
        return;
    }
    SecureWipe(ptr, nBytes);
    g_pfnSecureFree.load()(ptr);
}
} // namespace util
} // namespace bls
