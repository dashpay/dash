// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_COLLATERAL_H
#define BITCOIN_GOVERNANCE_COLLATERAL_H

#include <consensus/amount.h>
#include <serialize.h>
#include <uint256.h>

#include <optional>
#include <utility>
#include <vector>

class CTransaction;

static constexpr CAmount GOVERNANCE_PROPOSAL_FEE_TX = (1 * COIN);

namespace governance {
/** The parts of a mined transaction that decide whether it is valid proposal collateral. */
struct CollateralInfo {
    uint256 block_hash;
    //! Whether every output pays to P2PKH or is unspendable.
    bool outputs_standard{false};
    //! Value burned to each `OP_RETURN <32-byte hash>` output, together with that hash.
    std::vector<std::pair<uint256, CAmount>> burns;

    SERIALIZE_METHODS(CollateralInfo, obj) { READWRITE(obj.block_hash, obj.outputs_standard, obj.burns); }

    bool BurnsAtLeast(const uint256& hash, CAmount amount) const;
};

CollateralInfo GetCollateralInfo(const CTransaction& tx, const uint256& block_hash);

/** Where proposal collateral is looked up. Implemented by GovCollateralIndex. */
class CollateralSource
{
public:
    virtual ~CollateralSource() = default;

    //! Whether every block up to the active tip has been read. Until then, a transaction that isn't
    //! found may still be valid collateral.
    virtual bool IsCollateralSynced() const = 0;

    virtual std::optional<CollateralInfo> FindCollateral(const uint256& txid) const = 0;
};
} // namespace governance

#endif // BITCOIN_GOVERNANCE_COLLATERAL_H
