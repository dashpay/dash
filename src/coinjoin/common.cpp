// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinjoin/common.h>

#include <bls/bls.h>
#include <core_io.h>
#include <logging.h>
#include <univalue.h>

namespace CoinJoin
{
std::string DenominationToString(int nDenom)
{
    switch (CAmount nDenomAmount = DenominationToAmount(nDenom)) {
        case  0: return "N/A";
        case -1: return "out-of-bounds";
        case -2: return "non-denom";
        case -3: return "to-amount-error";
        default: return ValueFromAmount(nDenomAmount).getValStr();
    }

    // shouldn't happen
    return "to-string-error";
}

} // namespace CoinJoin
  //
uint256 CCoinJoinQueue::GetSignatureHash() const
{
    return SerializeHash(*this, SER_GETHASH, PROTOCOL_VERSION);
}
uint256 CCoinJoinQueue::GetHash() const { return SerializeHash(*this, SER_NETWORK, PROTOCOL_VERSION); }

bool CCoinJoinQueue::CheckSignature(const CBLSPublicKey& blsPubKey) const
{
    if (!CBLSSignature(Span{vchSig}, false).VerifyInsecure(blsPubKey, GetSignatureHash(), false)) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinQueue::CheckSignature -- VerifyInsecure() failed\n");
        return false;
    }

    return true;
}

bool CCoinJoinQueue::IsTimeOutOfBounds(int64_t current_time) const
{
    return current_time - nTime > COINJOIN_QUEUE_TIMEOUT ||
           nTime - current_time > COINJOIN_QUEUE_TIMEOUT;
}

[[nodiscard]] std::string CCoinJoinQueue::ToString() const
{
    return strprintf("nDenom=%d, nTime=%lld, fReady=%s, fTried=%s, masternode=%s",
        nDenom, nTime, fReady ? "true" : "false", fTried ? "true" : "false", masternodeOutpoint.ToStringShort());
}

