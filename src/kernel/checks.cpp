// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/checks.h>

#include <bls/bls.h>
#include <key.h>
#include <random.h>

namespace kernel {

std::optional<SanityCheckError> SanityChecks(const Context&)
{
    if (!ECC_InitSanityCheck()) {
        return SanityCheckError::ERROR_ECC;
    }

    if (!BLSInit()) {
        return SanityCheckError::ERROR_BLS;
    }

    if (!Random_SanityCheck()) {
        return SanityCheckError::ERROR_RANDOM;
    }

    return std::nullopt;
}

}
