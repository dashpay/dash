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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PlatformService;

/**
 * Finds, creates and tops up the wallet's DashPay identities, as the mobile
 * wallets do. Each identity index of the seed registers at most one: the
 * identities are found by probing the MASTER key of index 0, 1, … until
 * GAP_LIMIT indexes in a row hold none, as rs-platform-wallet scans, and a
 * new one is created at the index after the highest found.
 *
 * One identity is selected, the main one as the mobile wallets keep it per
 * wallet; the other flows work on it alone. Switching it reads the identity
 * again, and is refused while any flow is busy, so no write crosses it.
 *
 * Creating or topping up builds an asset lock paying a key derived from the
 * seed, the user confirms its amount and exact fee before it is sent, and
 * once the node has the payment's InstantSend lock or ChainLock one SDK call
 * creates the identity or adds the credits.
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
    //! The indexes in a row without an identity that end the scan, as
    //! rs-platform-wallet's IDENTITY_GAP_LIMIT.
    static constexpr uint32_t GAP_LIMIT{5};
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
        //! The index of the identity it registers.
        uint32_t index{0};
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
    //! Whether the identities were looked up since the client started.
    bool lookedUp() const { return m_looked_up; }
    //! The identities found, by index.
    const std::map<uint32_t, platform::Identity>& identities() const { return m_identities; }
    //! The selected identity. It is nullptr between two, so whatever was
    //! read for one is reset as when a gate closes.
    const platform::Identity* identity() const { return m_index ? &m_identities.at(*m_index) : nullptr; }
    //! The index of identity().
    uint32_t index() const { return m_index.value_or(0); }
    bool isSelected(const platform::Identifier& id) const { return identity() && identity()->id == id; }
    //! The asset lock waiting for Platform, if any.
    std::optional<Funding> funding() const;
    //! Why the last step failed; empty when it did not.
    const QString& error() const { return m_error; }

    //! Look up the identities registered under the wallet's MASTER keys,
    //! asking for the passphrase if a key must be derived.
    void lookUp();
    //! Select the identity found at `index` and read it again.
    void select(uint32_t index);
    //! Fund and register an identity at the next index.
    void create();
    //! Fund `amount` duffs of credits for the selected identity.
    void topUp(CAmount amount);
    //! Send funding() to Platform again.
    void resume();

Q_SIGNALS:
    void changed();

private:
    void serviceChanged();
    void startLookUp(bool unlock);
    //! The MASTER key hash of `index`, derived unless kept; nullopt while the
    //! wallet is locked.
    std::optional<std::array<uint8_t, 20>> keyHash(uint32_t index);
    //! Look the identity at `index` up, after `misses` indexes in a row without one.
    void probe(uint32_t index, uint32_t misses);
    void onProbed(uint32_t index, uint32_t misses, platform::Result<platform::Identity> result);
    void onSelected(uint32_t index, platform::Result<platform::Identity> result);
    uint32_t nextIndex() const;
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
    std::map<uint32_t, platform::Identity> m_identities;
    std::optional<uint32_t> m_index;
    QString m_error;
    //! Hash160 of the MASTER key of each index from 0, kept once derived:
    //! deriving needs an unlocked wallet.
    std::vector<std::array<uint8_t, 20>> m_key_hashes;
    //! The one call in flight, until it calls back or is dropped.
    std::weak_ptr<void> m_call;
    //! The funding being waited for or sent, and the unlock it signs with;
    //! or the unlock a lookup derives the keys of later indexes under.
    std::optional<Funding> m_funding;
    CTransactionRef m_tx;
    std::optional<WalletModel::UnlockContext> m_unlock;
};

#endif // BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
