// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINJOIN_COMMON_H
#define BITCOIN_COINJOIN_COMMON_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <timedata.h>

#include <array>
#include <string>
#include <util/ranges.h>

class CBLSPublicKey;

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

constexpr bool IsCollateralAmount(CAmount nInputAmount)
{
    // collateral input can be anything between 1x and "max" (including both)
    return (nInputAmount >= GetCollateralAmount() && nInputAmount <= GetMaxCollateralAmount());
}

constexpr int CalculateAmountPriority(CAmount nInputAmount)
{
    if (auto optDenom = ranges::find_if_opt(GetStandardDenominations(), [&nInputAmount](const auto& denom) {
        return nInputAmount == denom;
    })) {
        return (float)COIN / *optDenom * 10000;
    }
    if (nInputAmount < COIN) {
        return 20000;
    }

    //nondenom return largest first
    return -1 * (nInputAmount / COIN);
}
} // namespace CoinJoin

/**
 * A currently in progress mixing merge and denomination information
 */
static constexpr int COINJOIN_QUEUE_TIMEOUT = 30;
class CCoinJoinQueue
// TODO: move it inside CoinJoin namespace
{
public:
    int nDenom{0};
    COutPoint masternodeOutpoint;
    uint256 m_protxHash;
    int64_t nTime{0};
    bool fReady{false}; //ready for submit
    std::vector<unsigned char> vchSig;
    // memory only
    bool fTried{false};

    CCoinJoinQueue() = default;

    CCoinJoinQueue(int nDenom, const COutPoint& outpoint, const uint256& proTxHash, int64_t nTime, bool fReady) :
        nDenom(nDenom),
        masternodeOutpoint(outpoint),
        m_protxHash(proTxHash),
        nTime(nTime),
        fReady(fReady)
    {
    }

    SERIALIZE_METHODS(CCoinJoinQueue, obj)
    {
        READWRITE(obj.nDenom, obj.m_protxHash, obj.nTime, obj.fReady);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(obj.vchSig);
        }
    }

    [[nodiscard]] uint256 GetHash() const;
    [[nodiscard]] uint256 GetSignatureHash() const;

    /// Check if we have a valid Masternode address
    [[nodiscard]] bool CheckSignature(const CBLSPublicKey& blsPubKey) const;

    /// Check if a queue is too old or too far into the future
    [[nodiscard]] bool IsTimeOutOfBounds(int64_t current_time = GetAdjustedTime()) const;

    [[nodiscard]] std::string ToString() const;

    friend bool operator==(const CCoinJoinQueue& a, const CCoinJoinQueue& b)
    {
        return a.nDenom == b.nDenom && a.masternodeOutpoint == b.masternodeOutpoint && a.nTime == b.nTime && a.fReady == b.fReady;
    }
};

#endif // BITCOIN_COINJOIN_COMMON_H
