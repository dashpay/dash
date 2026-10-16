// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMUI_H
#define BITCOIN_QT_PLATFORM_PLATFORMUI_H

#include <platform/client.h>
#include <platform/types.h>
#include <qt/bitcoinunits.h>
#include <qt/walletmodel.h>

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace PlatformUi {
//! Credits in one duff, as Dash Platform converts an asset lock.
constexpr uint64_t CREDITS_PER_DUFF{1000};
//! A failed Platform call as the user reads it; empty for OK and NOT_FOUND,
//! which are answers.
QString Describe(const platform::Status& status);
//! A Platform balance in credits as a Dash amount, rounded down to a duff.
QString FormatCredits(BitcoinUnit unit, uint64_t credits);
//! The key a document is signed with: the identity's enabled HIGH
//! authentication key, whatever its id.
std::optional<uint32_t> DocumentSigningKey(const platform::Identity& identity);
//! Keeps the wallet unlocked until the client has signed the write.
std::unique_ptr<platform::UnlockScope> KeepUnlocked(WalletModel::UnlockContext unlock);
//! The callback of a call, owning a token `call` points to: it expires
//! once the client drops the call, which then never calls back.
template <typename T>
platform::PlatformClient::Callback<T> Tracked(std::weak_ptr<void>& call, std::function<void(platform::Result<T>)> fn)
{
    auto owner{std::make_shared<int>(0)};
    call = owner;
    return [owner, fn = std::move(fn)](platform::Result<T> result) { fn(std::move(result)); };
}
} // namespace PlatformUi

#endif // BITCOIN_QT_PLATFORM_PLATFORMUI_H
