// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformui.h>

#include <consensus/amount.h>

#include <QCoreApplication>

#include <utility>

namespace PlatformUi {
namespace {
class WalletUnlock final : public platform::UnlockScope
{
public:
    explicit WalletUnlock(WalletModel::UnlockContext unlock) :
        m_unlock(std::move(unlock))
    {
    }

private:
    WalletModel::UnlockContext m_unlock;
};
} // namespace

QString Describe(const platform::Status& status)
{
    using platform::StatusKind;
    switch (status.kind) {
    case StatusKind::OK:
    case StatusKind::NOT_FOUND:
        return {};
    case StatusKind::UNAVAILABLE:
        return QCoreApplication::translate("PlatformUi",
                                           "Dash Platform can't be reached right now. Try again in a moment.");
    case StatusKind::REJECTED:
        return QCoreApplication::translate("PlatformUi", "Dash Core could not verify Dash Platform's answer, so it was "
                                                         "not used. Try again in a minute.");
    case StatusKind::CONSENSUS:
        return QCoreApplication::translate("PlatformUi", "Dash Platform did not accept this (consensus error %1).")
            .arg(status.consensus_code);
    case StatusKind::INTERNAL:
        return QCoreApplication::translate("PlatformUi", "Something went wrong inside Dash Core: %1")
            .arg(QString::fromStdString(status.message));
    }
    return {};
}

bool IsAssetLockConsumed(const platform::Status& status)
{
    // rs-dpp IdentityAssetLockTransactionOutPointAlreadyConsumedError.
    constexpr uint32_t CODE_ASSET_LOCK_CONSUMED{10504};
    return status.kind == platform::StatusKind::CONSENSUS && status.consensus_code == CODE_ASSET_LOCK_CONSUMED;
}

QString FormatCredits(BitcoinUnit unit, uint64_t credits)
{
    return BitcoinUnits::formatWithUnit(unit, static_cast<CAmount>(credits / CREDITS_PER_DUFF));
}

std::optional<uint32_t> DocumentSigningKey(const platform::Identity& identity)
{
    using platform::IdentityPublicKey;
    for (const IdentityPublicKey& key : identity.public_keys) {
        if (key.purpose == IdentityPublicKey::Purpose::AUTHENTICATION &&
            key.security_level == IdentityPublicKey::SecurityLevel::HIGH &&
            key.type == IdentityPublicKey::Type::ECDSA_SECP256K1 && key.disabled_at == 0) {
            return key.id;
        }
    }
    return std::nullopt;
}

std::unique_ptr<platform::UnlockScope> KeepUnlocked(WalletModel::UnlockContext unlock)
{
    return std::make_unique<WalletUnlock>(std::move(unlock));
}
} // namespace PlatformUi
