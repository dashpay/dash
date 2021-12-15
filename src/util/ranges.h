// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_RANGES_H
#define BITCOIN_UTIL_RANGES_H

#include <algorithm>

//#if __cplusplus > 201703L // C++20 compiler
//namespace ranges = std::ranges;
//#else

#define MK_RANGE(FUN) \
template <typename X, typename Z>               \
inline auto FUN(const X& ds, const Z& fn) {     \
    return std::FUN(cbegin(ds), cend(ds), fn);  \
}

namespace ranges {
    MK_RANGE(all_of)
    MK_RANGE(any_of)
}

//#endif // C++20 compiler
#endif // BITCOIN_UTIL_RANGES_H
