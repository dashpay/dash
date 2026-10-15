// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WARNINGS_H
#define BITCOIN_WARNINGS_H

#include <optional>
#include <string>

struct bilingual_str;

void SetMiscWarning(const bilingual_str& warning);
/** Set or, with nullopt, clear the "snapshot state unavailable" warning of one
 *  source (a stalled chainstate or a failing operation). These do not replace
 *  the misc warning, and each lasts only while its source fails. Returns
 *  whether the warnings changed. */
bool SetEvoStateWarning(const std::string& source, const std::optional<bilingual_str>& warning);
void SetfLargeWorkInvalidChainFound(bool flag);
/** Format a string that describes several potential problems detected by the core.
 * @param[in] verbose bool
 * - if true, get all warnings separated by <hr />
 * - if false, get the most important warning
 * @returns the warning string
 */
bilingual_str GetWarnings(bool verbose);

#endif // BITCOIN_WARNINGS_H
