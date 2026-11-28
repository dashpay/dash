// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/context.h>

#include <crypto/sha256.h>
#include <crypto/x11/dispatch.h>
#include <key.h>
#include <logging.h>
#include <pubkey.h>
#include <random.h>

#include <mutex>
#include <string>

namespace kernel {
Context* g_context;

Context::Context()
{
    assert(!g_context);
    g_context = this;
    static std::once_flag globals_initialized{};
    std::call_once(globals_initialized, []() {
        SapphireAutoDetect();
        std::string sha256_algo = SHA256AutoDetect();
        LogInfo("Using the '%s' SHA256 implementation\n", sha256_algo);
        RandomInit();
    });
    ECC_Start();
}

Context::~Context()
{
    ECC_Stop();
    assert(g_context);
    g_context = nullptr;
}

} // namespace kernel
