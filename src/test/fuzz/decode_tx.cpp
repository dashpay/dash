// Copyright (c) 2019-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_io.h>
#include <primitives/transaction.h>
#include <test/fuzz/fuzz.h>
#include <util/strencodings.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

FUZZ_TARGET(decode_tx)
{
    // Dash's DecodeHexTx takes no try_no_witness/try_witness flags (no witness support),
    // so unlike upstream we can't assert all decode attempts fail — a fuzzer-generated
    // buffer can coincidentally be a valid serialized transaction.
    const std::string tx_hex = HexStr(std::string{buffer.begin(), buffer.end()});
    CMutableTransaction mtx;
    (void)DecodeHexTx(mtx, tx_hex);
}
