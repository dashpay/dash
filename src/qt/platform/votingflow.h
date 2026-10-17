// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_VOTINGFLOW_H
#define BITCOIN_QT_PLATFORM_VOTINGFLOW_H

#include <platform/types.h>
#include <qt/platform/platformservice.h>
#include <uint256.h>

#include <QObject>
#include <QString>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * The masternode vote on contested DashPay usernames, cast with the wallet's
 * masternodes: the valid ones in Core's masternode list whose voting key the
 * wallet holds. Each vote is signed in the wallet with that key.
 *
 * The open contests and our masternodes' votes are read from Platform only
 * once the user asks, which on a wallet that has not started DashPay is its
 * first DashPay click. No identity is needed.
 */
class VotingFlow : public QObject
{
    Q_OBJECT

public:
    enum class Step {
        IDLE,
        VOTING, //!< from the passphrase to Platform's answer
    };
    using Masternode = PlatformService::Masternode;

    explicit VotingFlow(PlatformService& service);

    Step step() const { return m_step; }
    //! The wallet's masternodes, in the masternode list's order.
    const std::vector<Masternode>& masternodes() const { return m_masternodes; }
    //! The open contests; nullopt until read since load().
    const std::optional<std::vector<platform::Contest>>& contests() const { return m_contests; }
    //! A masternode's votes; nullptr until read, or when reading them failed.
    const std::vector<platform::NameVote>* votes(const uint256& pro_tx_hash) const;
    //! The tally of the contest last opened with readTally(), and its label.
    const std::optional<platform::VoteState>& tally() const { return m_tally; }
    const std::string& tallyLabel() const { return m_tally_label; }
    //! Why the last read or vote failed; empty when none did.
    const QString& error() const { return m_error; }

    //! Read the open contests and our masternodes' votes, starting DashPay
    //! for the wallet first if it has not been.
    void load();
    //! Read the tally of a contest, keeping the one shown until it answers.
    void readTally(const std::string& label);
    //! Cast `vote` as our masternode pro_tx_hash. By value: the passphrase
    //! dialog may change what references point into.
    void vote(uint256 pro_tx_hash, platform::NameVote vote);

Q_SIGNALS:
    void changed();

private:
    void update();
    void readMasternodes();
    //! Issue the reads asked for that have no answer and none in flight.
    void read();
    void onVotes(const uint256& pro_tx_hash, platform::Result<std::vector<platform::NameVote>> result);
    void onTally(const std::string& label, platform::Result<platform::VoteState> result);
    void onVoted(const uint256& pro_tx_hash, const std::string& label, const platform::Status& status);
    void finish(const QString& error = {});

    PlatformService& m_service;
    Step m_step{Step::IDLE};
    std::vector<Masternode> m_masternodes;
    //! load() was asked for since DashPay was started.
    bool m_wanted{false};
    std::optional<std::vector<platform::Contest>> m_contests;
    //! Each masternode whose votes were answered; nullopt: the read failed.
    std::map<uint256, std::optional<std::vector<platform::NameVote>>> m_votes;
    std::string m_tally_label;
    std::optional<platform::VoteState> m_tally;
    //! The tally of m_tally_label was answered since last asked for.
    bool m_tally_read{false};
    //! The calls in flight, until they call back or are dropped.
    std::weak_ptr<void> m_call;
    std::weak_ptr<void> m_contests_call;
    std::map<uint256, std::weak_ptr<void>> m_votes_calls;
    std::weak_ptr<void> m_tally_call;
    QString m_error;
};

#endif // BITCOIN_QT_PLATFORM_VOTINGFLOW_H
