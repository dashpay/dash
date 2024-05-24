// Copyright (c) 2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSTEXPR_IF_CXX20_H
#define BITCOIN_CONSTEXPR_IF_CXX20_H

#if __cplusplus >= 202002L
#define CONSTEXPR_IF_CPP20 constexpr
#else
#define CONSTEXPR_IF_CPP20
#endif

#endif //BITCOIN_CONSTEXPR_IF_CXX20_H
