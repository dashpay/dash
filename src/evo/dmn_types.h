// Copyright (c) 2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_DMN_TYPES_H
#define BITCOIN_EVO_DMN_TYPES_H

#include <amount.h>
#include <cassert>

class CDeterministicMNType
{
public:
    uint8_t index;
    int32_t voting_weight;
    CAmount collat_amount;

//public:
//    constexpr CDeterministicMNType(int32_t vot, CAmount col) {
//        voting_weight = vot;
//        collat_amount = col;
//    }
//
//    int32_t getVotingWeight() {
//        return voting_weight;
//    }
//    CAmount getCollateralAmount() {
//        return collat_amount;
//    }
};

namespace MnType {
    constexpr auto Regular = CDeterministicMNType{
        .index = 0,
        .voting_weight = 1,
        .collat_amount = 1000,
    };
    constexpr auto HighPerformance = CDeterministicMNType{
        .index = 1,
        .voting_weight = 4,
        .collat_amount = 4000,
    };
}

constexpr const auto& GetMnType(int index) {
    switch (index) {
        case 0: return MnType::Regular;
        case 1: return MnType::HighPerformance;
        default: assert(false);
    }
}

// Ensure that these are in the order of index
//std::array<CDeterministicMNType, 2> MnTypes {
//    MnType::Regular,
//    MnType::HighPerformance
//};

#endif //BITCOIN_EVO_DMN_TYPES_H
