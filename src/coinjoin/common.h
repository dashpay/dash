// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINJOIN_COMMON_H
#define BITCOIN_COINJOIN_COMMON_H

#include <util/helpers.h>

#include <consensus/amount.h>
#include <primitives/transaction.h>

#include <array>
#include <string>

/** Holds a mixing input
 */
class CTxDSIn : public CTxIn
{
public:
    // memory only
    CScript prevPubKey;
    bool fHasSig{false}; // flag to indicate if signed
    int nRounds{-10};

    CTxDSIn(const CTxIn& txin, CScript script, int nRounds) :
        CTxIn(txin),
        prevPubKey(std::move(script)),
        nRounds(nRounds)
    {
    }

    CTxDSIn() = default;
};

namespace CoinJoin
{

constexpr std::array<CAmount, 5> vecStandardDenominations{
        (10 * COIN) + 10000,
        (1 * COIN) + 1000,
        (COIN / 10) + 100,
        (COIN / 100) + 10,
        (COIN / 1000) + 1,
};

constexpr std::array<CAmount, 5> GetStandardDenominations() { return vecStandardDenominations; }
constexpr CAmount GetSmallestDenomination() { return vecStandardDenominations.back(); }

/*
    Return a bitshifted integer representing a denomination in vecStandardDenominations
    or 0 if none was found
*/
constexpr int AmountToDenomination(CAmount nInputAmount)
{
    for (size_t i = 0; i < vecStandardDenominations.size(); ++i) {
        if (nInputAmount == vecStandardDenominations[i]) {
            return 1 << i;
        }
    }
    return 0;
}

/*
    Returns:
    - one of standard denominations from vecStandardDenominations based on the provided bitshifted integer
    - 0 for non-initialized sessions (nDenom = 0)
    - a value below 0 if an error occurred while converting from one to another
*/
constexpr CAmount DenominationToAmount(int nDenom)
{
    if (nDenom == 0) {
        // not initialized
        return 0;
    }

    size_t nMaxDenoms = vecStandardDenominations.size();

    if (nDenom >= (1 << nMaxDenoms) || nDenom < 0) {
        // out of bounds
        return -1;
    }

    if ((nDenom & (nDenom - 1)) != 0) {
        // non-denom
        return -2;
    }

    CAmount nDenomAmount{-3};

    for (size_t i = 0; i < nMaxDenoms; ++i) {
        if (nDenom & (1 << i)) {
            nDenomAmount = vecStandardDenominations[i];
            break;
        }
    }

    return nDenomAmount;
}


constexpr bool IsDenominatedAmount(CAmount nInputAmount) { return AmountToDenomination(nInputAmount) > 0; }
constexpr bool IsValidDenomination(int nDenom) { return DenominationToAmount(nDenom) > 0; }

/*
Same as DenominationToAmount but returns a string representation
*/
std::string DenominationToString(int nDenom);

constexpr CAmount GetCollateralAmount() { return GetSmallestDenomination() / 10; }
constexpr CAmount GetMaxCollateralAmount() { return GetCollateralAmount() * 4; }

// Promotion/demotion constants (post-V24 feature)
constexpr int PROMOTION_RATIO = 10;   // 10 smaller denomination coins = 1 larger denomination coin
constexpr int GAP_THRESHOLD = 10;     // Deficit gap required to trigger promotion/demotion

/**
 * Get the index of a denomination in vecStandardDenominations (0=largest, 4=smallest)
 * Returns -1 if not a valid denomination
 */
constexpr int GetDenominationIndex(int nDenom)
{
    if (nDenom <= 0) return -1;
    for (size_t i = 0; i < vecStandardDenominations.size(); ++i) {
        if (nDenom == (1 << i)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/**
 * Check if two denominations are adjacent (one step apart in the denom list)
 * Used for validating promotion/demotion entries post-V24
 */
constexpr bool AreAdjacentDenominations(int nDenom1, int nDenom2)
{
    int idx1 = GetDenominationIndex(nDenom1);
    int idx2 = GetDenominationIndex(nDenom2);
    if (idx1 < 0 || idx2 < 0) return false;
    return (idx1 == idx2 + 1) || (idx1 == idx2 - 1);
}

/**
 * Get the larger adjacent denomination (returns 0 if none exists or invalid)
 */
constexpr int GetLargerAdjacentDenom(int nDenom)
{
    int idx = GetDenominationIndex(nDenom);
    if (idx <= 0) return 0;  // Already largest or invalid
    return 1 << (idx - 1);
}

/**
 * Get the smaller adjacent denomination (returns 0 if none exists or invalid)
 */
constexpr int GetSmallerAdjacentDenom(int nDenom)
{
    int idx = GetDenominationIndex(nDenom);
    if (idx < 0 || idx >= static_cast<int>(vecStandardDenominations.size()) - 1) return 0;
    return 1 << (idx + 1);
}

constexpr bool IsCollateralAmount(CAmount nInputAmount)
{
    // collateral input can be anything between 1x and "max" (including both)
    return (nInputAmount >= GetCollateralAmount() && nInputAmount <= GetMaxCollateralAmount());
}

constexpr int CalculateAmountPriority(CAmount nInputAmount)
{
    if (auto optDenom = util::find_if_opt(GetStandardDenominations(),
                                          [&nInputAmount](const auto& denom) { return nInputAmount == denom; })) {
        return (float)COIN / *optDenom * 10000;
    }
    if (nInputAmount < COIN) {
        return 20000;
    }

    //nondenom return largest first
    return -1 * (nInputAmount / COIN);
}

} // namespace CoinJoin

#endif // BITCOIN_COINJOIN_COMMON_H
