// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMRECOVERY_H
#define BITCOIN_QT_PLATFORM_PLATFORMRECOVERY_H

#include <platform/types.h>

#include <QObject>

#include <cstdint>
#include <optional>
#include <vector>

class PlatformService;
class QThread;

/**
 * Seed-only recovery of Dash Platform state. A wallet restored from its
 * recovery phrase re-derives every private key but has none of the local
 * platform records. When no "identity/0" record exists, this probe rebuilds
 * them from proved Platform queries (mirroring the mobile wallets' shared
 * policy in rs-platform-wallet):
 *
 *   1. scan MASTER auth keys per identity index (gap limit of consecutive
 *      proven-absent indexes) via getIdentityByPublicKeyHash;
 *   2. synthesize the identity record — REGISTERED when namesOfIdentity
 *      proves a username, IDENTITY_CONFIRMED (no label) otherwise so the
 *      GUI offers name registration;
 *   3. restore established contacts (a request proved in both directions),
 *      decrypting their xpub from the incoming request and re-importing the
 *      friendship keychains;
 *   4. rebuild per-contact outbound payment cursors from wallet history and
 *      start a wallet rescan so pre-restore inbound payments appear.
 *
 * Network failures are never treated as proof of absence: an unanswered
 * probe finishes the session "incomplete" and the next start retries. The
 * probe runs at most once per session and never blocks the wallet.
 */
class PlatformRecovery : public QObject
{
    Q_OBJECT

public:
    explicit PlatformRecovery(PlatformService& service, QObject* parent = nullptr);
    ~PlatformRecovery() override;

    //! Begin the probe if it has not run this session. PlatformService calls
    //! this after every node-context refresh; it is a no-op until the client
    //! has endpoints and the wallet can derive platform keys (an encrypted
    //! wallet may only be unlocked later), and permanently once an identity
    //! record exists.
    void maybeStart();

Q_SIGNALS:
    //! Emitted once, on the GUI thread, when the probe ends. recovered is
    //! true when an identity record was synthesized.
    void finished(bool recovered);

private:
    struct RestoredContact {
        platform::Identifier identity{};
        std::vector<uint8_t> xpub; //!< 33-byte pubkey || 32-byte chain code
    };

    void probeIndex(uint32_t index, uint32_t consecutive_absent);
    void restoreIdentity();
    void restoreContacts();
    void restoreNextContact();
    void rebuildPayCursors();
    void startRescan();
    void finish(bool recovered, const std::string& outcome);

    PlatformService& m_service;
    bool m_attempted{false};
    //! First identity the scan proved to exist (the one recovered).
    std::optional<uint32_t> m_found_index;
    platform::Identifier m_identity_id{};
    uint32_t m_extra_identities{0};
    //! Established contacts pending restore (their incoming request).
    std::vector<platform::ContactRequest> m_established;
    std::vector<RestoredContact> m_restored;
    QThread* m_rescan_thread{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMRECOVERY_H
