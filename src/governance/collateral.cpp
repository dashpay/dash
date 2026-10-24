// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <governance/collateral.h>

#include <primitives/transaction.h>
#include <script/script.h>
#include <span.h>

#include <algorithm>

namespace governance {
bool CollateralInfo::BurnsAtLeast(const uint256& hash, CAmount amount) const
{
    return std::any_of(burns.begin(), burns.end(),
                       [&](const auto& burn) { return burn.first == hash && burn.second >= amount; });
}

CollateralInfo GetCollateralInfo(const CTransaction& tx, const uint256& block_hash)
{
    CollateralInfo info{block_hash, /*outputs_standard=*/true, /*burns=*/{}};
    for (const auto& output : tx.vout) {
        const CScript& script{output.scriptPubKey};
        if (!script.IsPayToPublicKeyHash() && !script.IsUnspendable()) {
            info.outputs_standard = false;
        }
        // Exactly the script `CScript() << OP_RETURN << ToByteVector(hash)` produces
        if (script.size() == 2 + uint256::size() && script[0] == OP_RETURN && script[1] == uint256::size()) {
            info.burns.emplace_back(uint256{Span{script}.subspan(2)}, output.nValue);
        }
    }
    return info;
}
} // namespace governance
