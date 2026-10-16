// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
#define BITCOIN_QT_PLATFORM_IDENTITYFLOW_H

#include <consensus/amount.h>
#include <platform/client.h>
#include <platform/types.h>
#include <primitives/transaction.h>
#include <qt/walletmodel.h>
#include <uint256.h>
#include <wallet/platformtypes.h>

#include <QObject>
#include <QString>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class PlatformService;

/**
 * Creates and tops up the wallet's DashPay identity (identity index 0), as
 * the mobile wallets do: the wallet builds an asset lock paying a key derived
 * from its seed, the user confirms its amount and exact fee before it is
 * sent, and once the node has the payment's InstantSend lock or ChainLock one
 * SDK call creates the identity or adds the credits.
 *
 * The asset lock's wallet record is written before it is sent and erased
 * once Platform has consumed it, so funding interrupted by a restart, a
 * dropped call or a refusal resumes from the record (Continue). An asset
 * lock Platform reports as already consumed was consumed by an earlier
 * attempt: the identity is then looked up by its MASTER key.
 *
 * The passphrase given to fund keeps the wallet unlocked until the client
 * has signed: while the node waits for the payment's lock, which may take
 * until its ChainLock, and until a closed gate ends the wait.
 */
class IdentityFlow : public QObject
{
    Q_OBJECT

public:
    //! Every identity is index 0 until the UI offers more than one.
    static constexpr uint32_t IDENTITY_INDEX{0};
    //! What a new identity is funded with, as the mobile wallets: enough to
    //! register a username.
    static constexpr CAmount REGISTRATION_AMOUNT{3'000'000};
    //! The smallest top-up the mobile wallets offer.
    static constexpr CAmount MIN_TOP_UP{1'000'000};
    //! The wallet records of asset locks Platform has not consumed yet.
    static constexpr const char* FUNDING_PREFIX{"dashpay/funding/"};

    //! Whether the user sends an asset lock buying `amount` duffs of credits
    //! for a network fee of `fee`.
    using Confirm = std::function<bool(CAmount amount, CAmount fee)>;
    //! The proof Platform takes for a committed asset lock; nullopt until
    //! the node has a lock for it.
    using Proof = std::function<std::optional<platform::AssetLockProof>(const CTransactionRef& tx)>;

    //! An asset lock this wallet sent that Platform has not consumed.
    struct Funding {
        std::string record;
        //! nullopt: it registers the identity.
        std::optional<wallet::TopupFundingKey> top_up;
        uint256 txid;
        CAmount amount{0};
        //! The identity a top-up adds the credits to.
        platform::Identifier identity{};
    };

    enum class Step {
        IDLE,
        LOOKING_UP,
        FUNDING,          //!< building the asset lock, asking for the passphrase or the confirmation
        WAITING_FOR_LOCK, //!< committed; the node has no lock for it yet
        SENDING,
    };

    //! An empty proof uses the node: the payment's InstantSend lock, else its
    //! ChainLock.
    IdentityFlow(PlatformService& service, Confirm confirm, Proof proof = {});

    Step step() const { return m_step; }
    //! Whether the identity was looked up since the client started.
    bool lookedUp() const { return m_looked_up; }
    const std::optional<platform::Identity>& identity() const { return m_identity; }
    //! The asset lock waiting for Platform, if any.
    std::optional<Funding> funding() const;
    //! Why the last step failed; empty when it did not.
    const QString& error() const { return m_error; }

    //! Look up the identity registered under the wallet's MASTER key,
    //! asking for the passphrase if the key must be derived.
    void lookUp();
    //! Fund the identity with `amount` duffs of credits and register it.
    void create(CAmount amount = REGISTRATION_AMOUNT);
    //! Fund `amount` duffs of credits for the identity.
    void topUp(CAmount amount);
    //! Send funding() to Platform again.
    void resume();

Q_SIGNALS:
    void changed();

private:
    void serviceChanged();
    void startLookUp(bool unlock);
    void onLookedUp(platform::Result<platform::Identity> result);
    void fund(const wallet::AssetLockFundingKey& key, CAmount amount, Funding pending);
    //! Ask for the passphrase; nullopt (and IDLE again) if the wallet stays locked.
    std::optional<WalletModel::UnlockContext> beginFunding();
    void awaitLock(Funding pending, CTransactionRef tx, WalletModel::UnlockContext unlock);
    //! Send the funding to Platform once the node has a lock for it and a client.
    void send();
    void sent(const Funding& funding, const platform::Status& status);
    void finish(const QString& error = {});

    PlatformService& m_service;
    const Confirm m_confirm;
    Proof m_proof;
    Step m_step{Step::IDLE};
    bool m_looked_up{false};
    std::optional<platform::Identity> m_identity;
    QString m_error;
    //! Hash160 of the MASTER key, kept once derived: deriving needs an
    //! unlocked wallet.
    std::optional<std::array<uint8_t, 20>> m_key_hash;
    //! The one call in flight, until it calls back or is dropped.
    std::weak_ptr<void> m_call;
    //! The funding being waited for or sent, and the unlock it signs with.
    std::optional<Funding> m_funding;
    CTransactionRef m_tx;
    std::optional<WalletModel::UnlockContext> m_unlock;
};

#endif // BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
