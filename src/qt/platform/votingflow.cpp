// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/votingflow.h>

#include <interfaces/wallet.h>
#include <logging.h>
#include <qt/clientfeeds.h>
#include <qt/clientmodel.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>
#include <script/standard.h>

#include <algorithm>
#include <utility>

namespace {
//! Platform's own words say why a vote was refused, as the mobile wallets
//! show them: already voted, too many changes, or what is wrong with the
//! voting key.
QString VoteError(const platform::Status& status)
{
    const QString error{PlatformUi::Describe(status)};
    if (status.kind != platform::StatusKind::CONSENSUS || status.message.empty()) return error;
    return QStringLiteral("%1 %2").arg(error, QString::fromStdString(status.message));
}
} // namespace

VotingFlow::VotingFlow(PlatformService& service) :
    m_service(service)
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &VotingFlow::update);
    connect(m_service.clientModel().feedMasternode(), &FeedBase::dataReady, this, &VotingFlow::readMasternodes);
    readMasternodes();
}

const std::vector<platform::NameVote>* VotingFlow::votes(const uint256& pro_tx_hash) const
{
    const auto it{m_votes.find(pro_tx_hash)};
    return it != m_votes.end() && it->second ? &*it->second : nullptr;
}

void VotingFlow::readMasternodes()
{
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    std::vector<Masternode> ours;
    for (const Masternode& masternode : m_service.masternodes()) {
        if (wallet.isSpendable(PKHash{masternode.voting_key})) ours.push_back(masternode);
    }
    if (ours == m_masternodes) return;
    m_masternodes = std::move(ours);
    read();
    Q_EMIT changed();
}

void VotingFlow::update()
{
    if (!m_service.consented()) {
        m_wanted = false;
        m_contests.reset();
        m_votes.clear();
        m_tally_label.clear();
        m_tally.reset();
        m_error.clear();
    }
    // A call the client dropped never calls back. The dropped vote may have
    // landed: our masternodes' votes, read again, say.
    if (m_step == Step::VOTING && m_call.expired()) {
        m_votes.clear();
        return finish(tr("Dash Platform's answer to the vote was lost. Your masternodes' votes are read again."));
    }
    read();
    Q_EMIT changed();
}

void VotingFlow::load()
{
    if (m_step != Step::IDLE) return;
    m_wanted = true;
    m_contests.reset();
    m_votes.clear();
    m_error.clear();
    if (!m_service.consented() && !m_service.start()) {
        return finish(tr("The wallet could not save that DashPay is turned on."));
    }
    read();
    Q_EMIT changed();
}

void VotingFlow::read()
{
    platform::PlatformClient* client{m_service.client()};
    if (!m_wanted || !client) return;
    if (!m_contests && m_contests_call.expired()) {
        client->currentContests(PlatformUi::Tracked<std::vector<platform::Contest>>(m_contests_call, [this](auto result) {
            if (!result.ok()) {
                LogPrintf("DashPay: reading the username contests failed: %s\n", result.status.message);
                m_error = PlatformUi::Describe(result.status);
            }
            m_contests = std::move(result.value);
            Q_EMIT changed();
        }));
    }
    for (const Masternode& masternode : m_masternodes) {
        auto& call{m_votes_calls[masternode.pro_tx_hash]};
        if (m_votes.contains(masternode.pro_tx_hash) || !call.expired()) continue;
        client->masternodeVotes(masternode.pro_tx_hash, PlatformUi::Tracked<std::vector<platform::NameVote>>(
                                                            call, [this, pro_tx_hash = masternode.pro_tx_hash](auto result) {
                                                                onVotes(pro_tx_hash, std::move(result));
                                                            }));
    }
    if (!m_tally_label.empty() && !m_tally_read && m_tally_call.expired()) {
        client->contestedVoteState(m_tally_label,
                                   PlatformUi::Tracked<platform::VoteState>(m_tally_call,
                                                                            [this, label = m_tally_label](auto result) {
                                                                                onTally(label, std::move(result));
                                                                            }));
    }
}

void VotingFlow::onVotes(const uint256& pro_tx_hash, platform::Result<std::vector<platform::NameVote>> result)
{
    auto& votes{m_votes[pro_tx_hash]};
    if (result.ok()) {
        votes = std::move(result.value);
    } else {
        LogPrintf("DashPay: reading a masternode's votes failed: %s\n", result.status.message);
        votes.reset();
        m_error = PlatformUi::Describe(result.status);
    }
    Q_EMIT changed();
}

void VotingFlow::onTally(const std::string& label, platform::Result<platform::VoteState> result)
{
    // Another contest was opened meanwhile. This call's token lives until
    // its callback returns, so forget it for read() to ask again.
    if (label != m_tally_label) {
        m_tally_call.reset();
        return read();
    }
    m_tally_read = true;
    if (result.ok()) {
        m_tally = std::move(result.value);
    } else {
        m_error = PlatformUi::Describe(result.status);
    }
    Q_EMIT changed();
}

void VotingFlow::readTally(const std::string& label)
{
    if (label != m_tally_label) {
        m_tally_label = label;
        m_tally.reset();
    }
    m_tally_read = false;
    read();
    Q_EMIT changed();
}

void VotingFlow::vote(uint256 pro_tx_hash, platform::NameVote vote)
{
    const auto masternode{std::ranges::find(m_masternodes, pro_tx_hash, &Masternode::pro_tx_hash)};
    if (m_step != Step::IDLE || masternode == m_masternodes.end() || !m_service.client()) return;
    const CKeyID voting_key{masternode->voting_key};
    m_step = Step::VOTING;
    m_error.clear();
    // Not a dropped vote while the passphrase dialog is open.
    const auto dialog{std::make_shared<int>(0)};
    m_call = dialog;
    Q_EMIT changed();
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) return finish(tr("The wallet stayed locked, so no vote was cast."));
    // The passphrase dialog may have let another flow start, or closed a gate.
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay request is on its way. Try again once it is done."));
    }
    platform::PlatformClient* client{m_service.client()};
    if (!client) return finish();
    platform::Signing signing{m_service.walletModel().wallet(), /*identity_index=*/0,
                              PlatformUi::KeepUnlocked(std::move(unlock))};
    client->castMasternodeVote(pro_tx_hash, voting_key, vote, std::move(signing),
                               m_service.trackWrite<std::monostate>(PlatformUi::Tracked<std::monostate>(
                                   m_call, [this, pro_tx_hash, label = vote.normalized_label](auto result) {
                                       onVoted(pro_tx_hash, label, result.status);
                                   })));
}

void VotingFlow::onVoted(const uint256& pro_tx_hash, const std::string& label, const platform::Status& status)
{
    if (status.kind != platform::StatusKind::OK) {
        LogPrintf("DashPay: casting a masternode vote failed: %s\n", status.message);
        return finish(VoteError(status));
    }
    // Read back what Platform now holds, and the new tally.
    m_votes.erase(pro_tx_hash);
    m_votes_calls.erase(pro_tx_hash);
    if (label == m_tally_label) m_tally_read = false;
    finish();
}

void VotingFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_error = error;
    read();
    Q_EMIT changed();
}
