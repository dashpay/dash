// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_VALIDATORS_H
#define BITCOIN_GOVERNANCE_VALIDATORS_H

#include <string>

/**
 * Validate the serialized proposal data (hex-encoded JSON).
 * Returns true on success. On failure, returns false and fills strErrorOut
 * with a semicolon-delimited list of detected issues.
 */
bool ValidateProposal(const std::string& strDataHex, std::string& strErrorOut, bool fCheckExpiration = true,
                      bool fAllowScript = true);

#endif // BITCOIN_GOVERNANCE_VALIDATORS_H
