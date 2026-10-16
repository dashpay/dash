// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformui.h>

#include <consensus/amount.h>

#include <QCoreApplication>

#include <utility>

namespace PlatformUi {
namespace {
//! Credits in one duff, as Dash Platform converts an asset lock.
constexpr uint64_t CREDITS_PER_DUFF{1000};

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

QString FormatCredits(BitcoinUnit unit, uint64_t credits)
{
    return BitcoinUnits::formatWithUnit(unit, static_cast<CAmount>(credits / CREDITS_PER_DUFF));
}

std::unique_ptr<platform::UnlockScope> KeepUnlocked(WalletModel::UnlockContext unlock)
{
    return std::make_unique<WalletUnlock>(std::move(unlock));
}
} // namespace PlatformUi
