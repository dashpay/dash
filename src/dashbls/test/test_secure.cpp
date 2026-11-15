// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING.MIT or https://opensource.org/license/MIT

#include "bls.hpp"
#include "secure.h"

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
alignas(std::max_align_t) uint8_t g_arena[4096];
size_t g_arenaUsed{0};

void* ArenaAlloc(size_t nBytes)
{
    constexpr size_t nAlign = alignof(std::max_align_t);
    const size_t nStep = (nBytes + nAlign - 1) & ~(nAlign - 1);
    if (nStep > sizeof(g_arena) - g_arenaUsed) {
        return nullptr;
    }
    void* pBlock = g_arena + g_arenaUsed;
    g_arenaUsed += nStep;
    return pBlock;
}

void ArenaFree(void*) {}

struct ArenaGuard {
    ArenaGuard()
    {
        bls::util::GetSecureAllocator(&m_pfnAlloc, &m_pfnFree);
        g_arenaUsed = 0;
        bls::util::SetSecureAllocator(ArenaAlloc, ArenaFree);
    }

    ~ArenaGuard()
    {
        if (m_pfnAlloc != nullptr && m_pfnFree != nullptr) {
            bls::util::SetSecureAllocator(m_pfnAlloc, m_pfnFree);
        }
    }

private:
    bls::Util::SecureAllocCallback m_pfnAlloc{nullptr};
    bls::Util::SecureFreeCallback m_pfnFree{nullptr};
};
} // anonymous namespace

TEST_CASE("util::SecureWipe clears a buffer")
{
    std::array<uint8_t, 32> buf;
    buf.fill(0xff);
    bls::util::SecureWipe(buf.data(), buf.size());
    REQUIRE(std::all_of(buf.begin(), buf.end(), [](uint8_t b) {
        return b == 0;
    }));

    // Degenerate arguments are ignored rather than faulting.
    bls::util::SecureWipe(nullptr, 16);
    bls::util::SecureWipe(buf.data(), 0);
}

TEST_CASE("Secure allocation is safe to use from several threads")
{
    // The callbacks are reached from every thread that holds a key, so this is
    // here to be run under a sanitizer rather than to assert an outcome.
    std::vector<std::thread> threads;

    // Reinstalling the same pair keeps every allocation's free valid, so the
    // only thing left to race is the callbacks themselves.
    bls::Util::SecureAllocCallback pfnAlloc{nullptr};
    bls::Util::SecureFreeCallback pfnFree{nullptr};
    bls::util::GetSecureAllocator(&pfnAlloc, &pfnFree);
    threads.emplace_back([pfnAlloc, pfnFree] {
        for (int j = 0; j < 250; ++j) {
            bls::util::SetSecureAllocator(pfnAlloc, pfnFree);
        }
    });

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([] {
            for (int j = 0; j < 250; ++j) {
                auto* p = bls::util::SecAlloc<uint8_t>(64);
                std::memset(p, 0x7e, 64);
                bls::util::SecFree(p, 64);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    SUCCEED("no allocation raced another");
}

namespace {
void* AllocA(size_t nBytes) { return std::malloc(nBytes); }
void FreeA(void* ptr) { std::free(ptr); }
void* AllocB(size_t nBytes) { return std::malloc(nBytes); }
void FreeB(void* ptr) { std::free(ptr); }

bool IsCoherent(bls::Util::SecureAllocCallback pfnAlloc,
                bls::Util::SecureFreeCallback pfnFree)
{
    return (pfnAlloc == AllocA && pfnFree == FreeA) ||
           (pfnAlloc == AllocB && pfnFree == FreeB);
}
} // anonymous namespace

TEST_CASE("Installing allocators concurrently never mixes their halves")
{
    bls::Util::SecureAllocCallback pfnAlloc{nullptr};
    bls::Util::SecureFreeCallback pfnFree{nullptr};
    bls::util::GetSecureAllocator(&pfnAlloc, &pfnFree);

    std::atomic<bool> fMixed{false};
    std::atomic<bool> fDone{false};
    std::thread reader([&] {
        while (!fDone.load()) {
            bls::Util::SecureAllocCallback pfnSeenAlloc{nullptr};
            bls::Util::SecureFreeCallback pfnSeenFree{nullptr};
            bls::util::GetSecureAllocator(&pfnSeenAlloc, &pfnSeenFree);
            const bool fOriginal =
                pfnSeenAlloc == pfnAlloc && pfnSeenFree == pfnFree;
            if (!fOriginal && !IsCoherent(pfnSeenAlloc, pfnSeenFree)) {
                fMixed = true;
            }
        }
    });
    std::thread installA([] {
        for (int j = 0; j < 20000; ++j) {
            bls::util::SetSecureAllocator(AllocA, FreeA);
        }
    });
    std::thread installB([] {
        for (int j = 0; j < 20000; ++j) {
            bls::util::SetSecureAllocator(AllocB, FreeB);
        }
    });
    installA.join();
    installB.join();
    fDone = true;
    reader.join();

    bls::Util::SecureAllocCallback pfnLastAlloc{nullptr};
    bls::Util::SecureFreeCallback pfnLastFree{nullptr};
    bls::util::GetSecureAllocator(&pfnLastAlloc, &pfnLastFree);
    bls::util::SetSecureAllocator(pfnAlloc, pfnFree);

    REQUIRE_FALSE(fMixed.load());
    REQUIRE(IsCoherent(pfnLastAlloc, pfnLastFree));
}

TEST_CASE("The secure allocator is never called through a null")
{
    REQUIRE_THROWS_AS(bls::util::SetSecureAllocator(nullptr, std::free),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(bls::util::SetSecureAllocator(std::malloc, nullptr),
                      std::invalid_argument);

    // Rejecting the bad pair must not have disturbed the installed one.
    auto* p = bls::util::SecAlloc<uint8_t>(32);
    REQUIRE(p != nullptr);
    bls::util::SecFree(p, 32);
}

TEST_CASE("Secure allocation failure throws rather than returning null")
{
    // The raw SecAlloc callers do not check for null, so a count that does not
    // fit in a size_t once scaled by the object size has to throw rather than
    // wrap into a small allocation the caller then writes past.
    REQUIRE_THROWS_AS(bls::util::SecAlloc<uint64_t>(
        std::numeric_limits<size_t>::max() / 4), std::bad_alloc);
    REQUIRE_THROWS_AS(bls::util::SecAlloc<uint32_t>(
        std::numeric_limits<size_t>::max()), std::bad_alloc);
}

namespace {
size_t g_nNullFrees{0};

void* NullOnZeroAlloc(size_t nBytes)
{
    return nBytes == 0 ? nullptr : std::malloc(nBytes);
}

void CountingFree(void* ptr)
{
    if (ptr == nullptr) {
        ++g_nNullFrees;
    }
    std::free(ptr);
}
} // anonymous namespace

TEST_CASE("Secure allocation handles malloc edge cases")
{
    bls::Util::SecureAllocCallback pfnAlloc{nullptr};
    bls::Util::SecureFreeCallback pfnFree{nullptr};
    bls::util::GetSecureAllocator(&pfnAlloc, &pfnFree);
    bls::util::SetSecureAllocator(NullOnZeroAlloc, CountingFree);

    // A zero-length request is not a failure even if the callback says null.
    void* p = nullptr;
    REQUIRE_NOTHROW(p = bls::util::SecMalloc(0));
    REQUIRE(p != nullptr);
    bls::util::SecFree(p, 0);

    g_nNullFrees = 0;
    bls::util::SecFree(nullptr, 16);
    REQUIRE(g_nNullFrees == 0);

    bls::util::SetSecureAllocator(pfnAlloc, pfnFree);
}

TEST_CASE("A private key has value semantics and clears itself")
{
    SECTION("Destruction leaves nothing behind")
    {
        // The key's scalar goes back through SecFree, which the installed
        // allocator need not clear, so the extent has to travel with it.
        std::memset(g_arena, 0xcd, sizeof(g_arena));
        {
            ArenaGuard guard;
            const std::vector<uint8_t> skBytes(
                bls::PrivateKey::PRIVATE_KEY_SIZE, 0x2a);
            bls::PrivateKey sk = bls::PrivateKey::FromByteVector(skBytes);
            REQUIRE(std::any_of(g_arena, g_arena + sizeof(blst_scalar),
                                [](uint8_t b) { return b != 0 && b != 0xcd; }));
        }

        REQUIRE(std::all_of(g_arena, g_arena + sizeof(blst_scalar),
                            [](uint8_t b) { return b == 0; }));
    }

    SECTION("Copy and assignment carry the value")
    {
        std::vector<uint8_t> vecWide(bls::PrivateKey::PRIVATE_KEY_SIZE, 0x5b);
        vecWide[0] = 0x0f;

        bls::PrivateKey sk = bls::PrivateKey::FromBytes(bls::Bytes(vecWide));
        const bls::PrivateKey copied = sk;
        REQUIRE(copied == sk);

        // Self-assignment must not disturb the value it is about to read.
        bls::PrivateKey& alias = sk;
        sk = alias;
        REQUIRE(sk.Serialize() == vecWide);

        // Nor may self-move release the key out from under itself.
        sk = std::move(alias);
        REQUIRE(sk.HasKeyData());
        REQUIRE(sk.Serialize() == vecWide);
    }

    SECTION("Assignment clears the value it replaces")
    {
        std::vector<uint8_t> vecWide(bls::PrivateKey::PRIVATE_KEY_SIZE, 0x5b);
        vecWide[0] = 0x0f;
        std::vector<uint8_t> vecNarrow(bls::PrivateKey::PRIVATE_KEY_SIZE, 0x00);
        vecNarrow[bls::PrivateKey::PRIVATE_KEY_SIZE - 1] = 0x01;

        std::memset(g_arena, 0xcd, sizeof(g_arena));
        {
            ArenaGuard guard;
            bls::PrivateKey wide =
                bls::PrivateKey::FromBytes(bls::Bytes(vecWide));
            const bls::PrivateKey narrow =
                bls::PrivateKey::FromBytes(bls::Bytes(vecNarrow));
            wide = narrow;

            REQUIRE(wide.Serialize() == vecNarrow);
            // 0x5b was only ever a byte of the wide value, so finding it now
            // would mean the assignment left some of it behind.
            REQUIRE(std::none_of(g_arena, g_arena + sizeof(blst_scalar),
                                 [](uint8_t b) { return b == 0x5b; }));
        }
    }
}

TEST_CASE("util::SecVector allocates through the secure allocator")
{
    static_assert(std::is_same<bls::util::SecVector<uint32_t>::allocator_type,
                               bls::util::SecureAllocator<uint32_t>>::value,
                  "SecVector has to carry the secure allocator");

    // Growth reallocates, so this walks allocate and deallocate rather than
    // only the one reservation a sized construction would make.
    bls::util::SecVector<uint32_t> words;
    for (uint32_t i = 0; i < 64; ++i) {
        words.push_back(i);
    }

    REQUIRE(words.size() == 64);
    REQUIRE(words.front() == 0);
    REQUIRE(words.back() == 63);
}

TEST_CASE("Secure release clears the bytes before handing them back")
{
    SECTION("util::SecVector, through the allocator")
    {
        std::memset(g_arena, 0xcd, sizeof(g_arena));
        {
            ArenaGuard guard;
            bls::util::SecVector<uint8_t> secret(64);
            std::fill(secret.begin(), secret.end(), 0xab);
        }
        REQUIRE(std::none_of(g_arena, g_arena + 64, [](uint8_t b) {
            return b == 0xab;
        }));
        REQUIRE(std::all_of(g_arena, g_arena + 64, [](uint8_t b) {
            return b == 0;
        }));
    }

    SECTION("util::SecPtr, through the deleter")
    {
        std::memset(g_arena, 0xcd, sizeof(g_arena));
        {
            ArenaGuard guard;
            bls::util::SecPtr<uint32_t> secret =
                bls::util::SecMake<uint32_t>(16);
            std::fill(secret.get(), secret.get() + 16, 0xabababab);
        }
        // The whole array, not just its first object, which a deleter holding
        // no count could not have reached.
        REQUIRE(std::all_of(g_arena, g_arena + 16 * sizeof(uint32_t),
                            [](uint8_t b) { return b == 0; }));
        REQUIRE(g_arena[16 * sizeof(uint32_t)] == 0xcd);
    }
}

TEST_CASE("A chain code has value semantics and clears itself")
{
    const std::vector<uint8_t> vecA(bls::ChainCode::SIZE, 0xa7);
    const std::vector<uint8_t> vecB(bls::ChainCode::SIZE, 0x3c);

    SECTION("Copy and assignment carry the value")
    {
        const bls::ChainCode a = bls::ChainCode::FromBytes(bls::Bytes(vecA));
        const bls::ChainCode copied = a;
        REQUIRE(copied == a);

        bls::ChainCode assigned = bls::ChainCode::FromBytes(bls::Bytes(vecB));
        assigned = a;
        REQUIRE(assigned == a);

        // Self-assignment must not lose the value.
        bls::ChainCode& alias = assigned;
        assigned = alias;
        REQUIRE(assigned == a);
    }

    SECTION("Destruction leaves nothing behind")
    {
        std::memset(g_arena, 0xcd, sizeof(g_arena));
        {
            ArenaGuard guard;
            // The chain code lands in the arena, so what the destructor does
            // to it stays readable once the scope ends.
            auto* pHeld = static_cast<bls::ChainCode*>(
                bls::util::SecMalloc(sizeof(bls::ChainCode)));
            new (pHeld)
                bls::ChainCode(bls::ChainCode::FromBytes(bls::Bytes(vecA)));
            REQUIRE(std::any_of(g_arena, g_arena + sizeof(bls::ChainCode),
                                [](uint8_t b) { return b != 0 && b != 0xcd; }));
            pHeld->~ChainCode();

            REQUIRE(std::all_of(g_arena, g_arena + sizeof(bls::ChainCode),
                                [](uint8_t b) { return b == 0; }));
            bls::util::SecFree(pHeld, sizeof(bls::ChainCode));
        }
    }

    SECTION("Printing leaves nothing behind")
    {
        const bls::ChainCode a = bls::ChainCode::FromBytes(bls::Bytes(vecA));
        std::memset(g_arena, 0xcd, sizeof(g_arena));
        std::ostringstream os;
        {
            ArenaGuard guard;
            os << a;
            REQUIRE(g_arenaUsed >= size_t{bls::ChainCode::SIZE});
        }

        REQUIRE(os.str() == bls::Util::HexStr(vecA));
        // The serialized copy is the first thing the arena hands out.
        REQUIRE(std::all_of(g_arena, g_arena + bls::ChainCode::SIZE,
                            [](uint8_t b) { return b == 0; }));
    }
}
