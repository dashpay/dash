// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING.MIT or https://opensource.org/license/MIT

#ifndef DASHBLS_SECURE_H
#define DASHBLS_SECURE_H

#include "util.hpp"

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace bls {
namespace util {
/**
 * Overwrites a buffer that held a secret with zeroes.
 *
 * A memset behind a barrier, as Bitcoin Core's memory_cleanse does it, so the
 * store is not dropped on a buffer nothing reads again. A null pointer or a
 * zero length is ignored.
 *
 * See "Dead Store Elimination (Still) Considered Harmful", Yang et al.,
 * USENIX Security 2017.
 *
 * @param   ptr  Start of the buffer to clear.
 * @param   len  How many bytes to clear.
 */
void SecureWipe(void* ptr, size_t len) noexcept;

/**
 * Names the allocator that SecMalloc and SecFree go through.
 *
 * pfnAlloc must return storage aligned for any type with alignment no greater
 * than alignof(std::max_align_t).
 *
 * Other threads may allocate meanwhile, but storage has to go back to the
 * allocator it came from, so swap only while none of it is outstanding.
 *
 * @param   pfnAlloc  Called to obtain memory, returning nullptr on failure.
 * @param   pfnFree   Called to release what pfnAlloc returned.
 */
void SetSecureAllocator(Util::SecureAllocCallback pfnAlloc,
                        Util::SecureFreeCallback pfnFree);

/**
 * Reports the allocator currently installed.
 *
 * @param   ppfnAlloc  Receives the installed allocation callback.
 * @param   ppfnFree   Receives the installed release callback.
 */
void GetSecureAllocator(Util::SecureAllocCallback* ppfnAlloc,
                        Util::SecureFreeCallback* ppfnFree);

/**
 * Allocates nBytes from the installed secure allocator.
 *
 * @param   nBytes  How many bytes to allocate.
 * @returns The allocation, never nullptr.
 * @throws  std::bad_alloc if the allocator cannot satisfy the request.
 */
void* SecMalloc(size_t nBytes);

/**
 * Clears nBytes at ptr and returns it to the installed allocator.
 *
 * @param   ptr     An allocation from SecMalloc or SecAlloc, or nullptr.
 * @param   nBytes  How much of it to clear, as handed to the allocation.
 */
void SecFree(void* ptr, size_t nBytes);

/**
 * Allocates storage for numTs objects, cleared when freed.
 *
 * @param   numTs  How many objects to make room for.
 * @returns The allocation, never nullptr.
 * @throws  std::bad_alloc if numTs objects do not fit in a size_t, or if the
 *          allocator cannot satisfy the request.
 */
template <class T>
T* SecAlloc(size_t numTs)
{
    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "SecAlloc cannot satisfy an over-aligned type");

    if (numTs > std::numeric_limits<size_t>::max() / sizeof(T)) {
        throw std::bad_alloc();
    }
    return static_cast<T*>(SecMalloc(sizeof(T) * numTs));
}

/**
 * An allocator over the library's secure allocation.
 */
template <typename T>
struct SecureAllocator {
    using value_type = T;

    SecureAllocator() = default;
    template <typename U>
    SecureAllocator(const SecureAllocator<U>&) noexcept {}

    /**
     * Allocates storage for n objects.
     *
     * @param   n  How many objects to make room for.
     * @returns Storage for n objects, cleared when deallocated.
     * @throws  std::bad_alloc if n objects do not fit in a size_t, or if the
     *          allocator cannot satisfy the request.
     */
    T* allocate(size_t n) { return SecAlloc<T>(n); }

    /**
     * Returns storage from allocate, clearing it on the way out.
     *
     * @param   p  Storage from a previous allocate.
     * @param   n  The count that allocate was given, so the bytes can be
     *             cleared before release rather than left to the allocator.
     */
    void deallocate(T* p, size_t n) { SecFree(p, n * sizeof(T)); }

    template <typename U>
    friend bool operator==(
        const SecureAllocator&, const SecureAllocator<U>&) noexcept
    {
        return true;
    }
    template <typename U>
    friend bool operator!=(
        const SecureAllocator&, const SecureAllocator<U>&) noexcept
    {
        return false;
    }
};

template <typename T>
using SecVector = std::vector<T, SecureAllocator<T>>;

/**
 * Returns storage from SecAlloc once it is no longer needed.
 *
 * It carries the count SecAlloc was given, as SecFree has to be told how many
 * bytes to clear and a unique_ptr does not remember it.
 */
template <class T>
struct SecDeleter {
    explicit SecDeleter(size_t nCountIn) : nCount(nCountIn) {}

    /**
     * Clears and releases the storage.
     *
     * @param   ptr  Storage from SecAlloc, or nullptr.
     */
    void operator()(T* ptr) const { SecFree(ptr, nCount * sizeof(T)); }

private:
    size_t nCount;
};

/**
 * Owns storage from SecAlloc, so a throw does not leak it.
 *
 * The storage is not constructed or destroyed, only cleared when released,
 * which suits the blst structures it is used for.
 */
template <class T>
using SecPtr = std::unique_ptr<T, SecDeleter<T>>;

/**
 * Allocates owned storage for numTs objects.
 *
 * @param   numTs  How many objects to make room for.
 * @returns Storage for numTs objects, cleared when it goes out of scope.
 * @throws  std::bad_alloc as SecAlloc does.
 */
template <class T>
SecPtr<T> SecMake(size_t numTs = 1)
{
    return SecPtr<T>(SecAlloc<T>(numTs), SecDeleter<T>{numTs});
}
} // namespace util
} // namespace bls

#endif // DASHBLS_SECURE_H
