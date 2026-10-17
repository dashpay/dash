// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/votingpage.h>

#include <base58.h>
#include <qt/guiutil.h>
#include <qt/platform/votingflow.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>

namespace {
using Choice = platform::NameVote::Choice;

//! A masternode's vote on a contest, if its votes were read and it voted.
const platform::NameVote* VoteOn(const std::vector<platform::NameVote>* votes, const std::string& label)
{
    if (!votes) return nullptr;
    const auto vote{std::find_if(votes->begin(), votes->end(),
                                 [&](const platform::NameVote& vote) { return vote.normalized_label == label; })};
    return vote != votes->end() ? &*vote : nullptr;
}

QString VoteText(const std::vector<platform::NameVote>* votes, const std::string& label)
{
    if (!votes) return VotingPage::tr("vote unknown");
    const platform::NameVote* vote{VoteOn(votes, label)};
    if (!vote) return VotingPage::tr("not voted");
    switch (vote->choice) {
    case Choice::TOWARDS_IDENTITY:
        return VotingPage::tr("voted for %1").arg(QString::fromStdString(EncodeBase58(vote->towards)));
    case Choice::ABSTAIN:
        return VotingPage::tr("abstained");
    case Choice::LOCK:
        return VotingPage::tr("voted to lock");
    }
    return {};
}
} // namespace

VotingPage::VotingPage(QWidget* parent) :
    QWidget(parent)
{
    auto* title{new QLabel(tr("Username voting"), this)};
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold);
    m_summary = new QLabel(this);
    m_summary->setObjectName("dashPayVotingSummary");
    m_summary->setWordWrap(true);
    m_load = new QPushButton(this);
    m_load->setObjectName("dashPayShowVotes");
    m_status = new QLabel(this);
    m_status->setObjectName("dashPayVotingStatus");
    m_status->setWordWrap(true);
    m_contests = new QListWidget(this);
    m_contests->setObjectName("dashPayContests");
    m_masternode = new QComboBox(this);
    m_masternode->setObjectName("dashPayVoteMasternode");
    m_choice = new QComboBox(this);
    m_choice->setObjectName("dashPayVoteChoice");
    m_cast = new QPushButton(tr("Vote"), this);
    m_cast->setObjectName("dashPayCastVote");
    m_rules = new QLabel(tr("Each masternode has one live vote per contest. You can change it later, but Dash "
                            "Platform allows only 5 votes in total per masternode per contest."),
                         this);
    m_rules->setWordWrap(true);

    auto* vote_row{new QHBoxLayout};
    vote_row->addWidget(m_masternode);
    vote_row->addWidget(m_choice);
    vote_row->addWidget(m_cast);
    vote_row->addStretch();
    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(title);
    layout->addWidget(m_summary);
    layout->addWidget(m_load, 0, Qt::AlignLeft);
    layout->addWidget(m_status);
    layout->addWidget(m_contests);
    layout->addLayout(vote_row);
    layout->addWidget(m_rules);

    connect(m_load, &QPushButton::clicked, this, [this] { m_flow->load(); });
    connect(m_contests, &QListWidget::currentRowChanged, this, [this] { m_flow->readTally(selectedLabel()); });
    connect(m_choice, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { m_cast->setEnabled(selectedChoice().has_value()); });
    connect(m_cast, &QPushButton::clicked, this, [this] {
        const int masternode{m_masternode->currentIndex()};
        const auto choice{selectedChoice()};
        if (masternode < 0 || !choice) return;
        m_flow->vote(m_flow->masternodes().at(masternode).pro_tx_hash, *choice);
    });
}

std::optional<platform::NameVote> VotingPage::selectedChoice() const
{
    // The first entry chooses nothing.
    const int index{m_choice->currentIndex() - 1};
    if (index < 0) return std::nullopt;
    return m_choices[index];
}

std::string VotingPage::selectedLabel() const
{
    const auto* item{m_contests->currentItem()};
    return item ? item->data(Qt::UserRole).toString().toStdString() : std::string{};
}

void VotingPage::refresh(bool writable)
{
    if (!m_flow) return;
    const auto& ours{m_flow->masternodes()};
    const size_t evonodes(std::ranges::count(ours, true, &VotingFlow::Masternode::evonode));
    m_summary->setText(tr("This wallet can vote with %1 evonodes and %2 masternodes: %3 votes per contest, as an "
                          "evonode counts as 4.")
                           .arg(evonodes)
                           .arg(ours.size() - evonodes)
                           .arg(ours.size() + 3 * evonodes));
    const bool idle{m_flow->step() == VotingFlow::Step::IDLE};
    const auto& contests{m_flow->contests()};
    m_load->setText(contests ? tr("Refresh") : tr("Show username votes"));
    m_load->setVisible(idle);

    const std::string selected{selectedLabel()};
    {
        const QSignalBlocker blocker{m_contests};
        m_contests->clear();
        if (contests) {
            for (const platform::Contest& contest : *contests) {
                const auto voted{std::count_if(ours.begin(), ours.end(), [&](const auto& masternode) {
                    return VoteOn(m_flow->votes(masternode.pro_tx_hash), contest.normalized_label) != nullptr;
                })};
                auto* item{new QListWidgetItem(tr("%1 — voting ends %2 — %3 of your %4 masternodes voted")
                                                   .arg(QString::fromStdString(contest.normalized_label),
                                                        GUIUtil::dateTimeStr(contest.ends_at_ms / 1000))
                                                   .arg(voted)
                                                   .arg(ours.size()),
                                               m_contests)};
                item->setData(Qt::UserRole, QString::fromStdString(contest.normalized_label));
                if (contest.normalized_label == selected) m_contests->setCurrentItem(item);
            }
        }
    }
    m_contests->setVisible(contests && !contests->empty());

    const std::string label{selectedLabel()};
    const auto& tally{m_flow->tally()};
    const bool shown{!label.empty() && m_flow->tallyLabel() == label && tally};
    const bool open{shown && tally->winner == platform::VoteState::Winner::NONE};
    QString status;
    if (!idle) {
        status = tr("Casting the vote…");
    } else if (!m_flow->error().isEmpty()) {
        status = m_flow->error();
    } else if (shown && !open) {
        status = tr("Voting on “%1” has closed. Refresh to see the result.").arg(QString::fromStdString(label));
    } else if (contests && contests->empty()) {
        status = tr("No usernames are being contested right now. Contests open when two or more people request the "
                    "same name.");
    }
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());

    {
        const QSignalBlocker blocker{m_masternode};
        const QVariant current{m_masternode->currentData()};
        m_masternode->clear();
        for (const auto& masternode : ours) {
            const QString pro_tx_hash{QString::fromStdString(masternode.pro_tx_hash.ToString())};
            m_masternode->addItem(tr("%1 %2… — %3")
                                      .arg(masternode.evonode ? tr("Evonode") : tr("Masternode"), pro_tx_hash.left(16),
                                           VoteText(m_flow->votes(masternode.pro_tx_hash), label)),
                                  pro_tx_hash);
        }
        m_masternode->setCurrentIndex(std::max(m_masternode->findData(current), 0));
    }

    {
        const QSignalBlocker blocker{m_choice};
        const auto current{selectedChoice()};
        m_choice->clear();
        m_choices.clear();
        m_choice->addItem(tr("Choose your vote"));
        const auto add = [&](platform::NameVote vote, const QString& text) {
            m_choices.push_back(std::move(vote));
            m_choice->addItem(text);
        };
        if (open) {
            for (const auto& contender : tally->contenders) {
                add({label, Choice::TOWARDS_IDENTITY, contender.identity},
                    tr("Award it to %1 (%2 votes)")
                        .arg(QString::fromStdString(EncodeBase58(contender.identity)))
                        .arg(contender.votes));
            }
            add({label, Choice::LOCK, {}}, tr("Lock the name, so nobody gets it (%1 votes)").arg(tally->lock));
            add({label, Choice::ABSTAIN, {}}, tr("Abstain, taking no side (%1 votes)").arg(tally->abstain));
        }
        const auto same{current ? std::ranges::find(m_choices, *current) : m_choices.end()};
        m_choice->setCurrentIndex(same == m_choices.end() ? 0 : 1 + static_cast<int>(same - m_choices.begin()));
    }
    m_cast->setEnabled(selectedChoice().has_value());
    const bool can_vote{writable && !m_choices.empty()};
    m_masternode->setVisible(can_vote);
    m_choice->setVisible(can_vote);
    m_cast->setVisible(can_vote);
    m_rules->setVisible(can_vote);
}
