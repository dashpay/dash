// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_SHAREDCOLLATERAL_H
#define BITCOIN_EVO_SHAREDCOLLATERAL_H

#include <script/script.h>

namespace sharedcollateral {

/**
 * The shared masternode collateral template script: 0x04 "DSHC" OP_DROP OP_TRUE
 * (hex 04445348437551). At the script layer this is anyone-can-spend; all
 * protection comes from consensus rules that restrict both creation (only as
 * the collateral output of a valid shared ProRegTx) and spending (only via a
 * valid ProDisTx). Matching is always exact-script comparison, never prefix
 * matching.
 */
inline const CScript& SharedCollateralScript()
{
    static const CScript script = CScript() << std::vector<unsigned char>{'D', 'S', 'H', 'C'} << OP_DROP << OP_TRUE;
    return script;
}

inline bool IsSharedCollateralScript(const CScript& script)
{
    return script == SharedCollateralScript();
}

} // namespace sharedcollateral

#endif // BITCOIN_EVO_SHAREDCOLLATERAL_H
