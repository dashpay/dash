// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sstream>
#include <stdio.h>
#include <tinyformat.h>
#include <util/bip32.h>
#include <util/strencodings.h>


std::string FormatHDKeypath(const std::vector<uint32_t>& path)
{
    std::string ret;
    for (auto i : path) {
        ret += strprintf("/%i", (i << 1) >> 1);
        if (i >> 31) ret += '\'';
    }
    return ret;
}

std::string WriteHDKeypath(const std::vector<uint32_t>& keypath)
{
    return "m" + FormatHDKeypath(keypath);
}
