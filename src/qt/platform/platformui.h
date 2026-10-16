// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMUI_H
#define BITCOIN_QT_PLATFORM_PLATFORMUI_H

#include <platform/types.h>
#include <qt/bitcoinunits.h>

#include <QString>

#include <cstdint>

namespace PlatformUi {
//! A failed Platform call as the user reads it; empty for OK and NOT_FOUND,
//! which are answers.
QString Describe(const platform::Status& status);
//! A Platform balance in credits as a Dash amount, rounded down to a duff.
QString FormatCredits(BitcoinUnit unit, uint64_t credits);
} // namespace PlatformUi

#endif // BITCOIN_QT_PLATFORM_PLATFORMUI_H
