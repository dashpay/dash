// Copyright (c) 2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/strencodings.h>

#include <string>
#include <vector>

std::string BitsToHexStr(const std::vector<bool>& vBits)
{
    std::vector<uint8_t> vBytes((vBits.size() + 7) / 8);
    for (size_t i = 0; i < vBits.size(); i++) {
        vBytes[i / 8] |= vBits[i] << (i % 8);
    }
    return HexStr(vBytes);
}
