// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/proposalvotedialog.h>

#include <qt/guiutil.h>

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

QString ProposalVoteOutcome(vote_outcome_enum_t outcome)
{
    switch (outcome) {
    case VOTE_OUTCOME_YES:
        return QObject::tr("Yes");
    case VOTE_OUTCOME_NO:
        return QObject::tr("No");
    case VOTE_OUTCOME_ABSTAIN:
        return QObject::tr("Abstain");
    default:
        return QObject::tr("Not voted");
    }
}

QString ProposalVotingDeadline(int height, int deadline, int64_t spacing)
{
    if (height >= deadline) {
        return QObject::tr("Voting deadline passed for this cycle (block %1)").arg(deadline);
    }
    return QObject::tr("Voting deadline: ~%1 left (%2 blocks, block %3)")
        .arg(GUIUtil::formatBlockDuration(deadline - height, spacing))
        .arg(deadline - height)
        .arg(deadline);
}

QString ProposalVoteSummary(const std::vector<ProposalVoter>& voters)
{
    int yes{0}, no{0}, abstain{0}, unvoted{0};
    for (const auto& voter : voters) {
        switch (voter.outcome) {
        case VOTE_OUTCOME_YES:
            yes += voter.weight;
            break;
        case VOTE_OUTCOME_NO:
            no += voter.weight;
            break;
        case VOTE_OUTCOME_ABSTAIN:
            abstain += voter.weight;
            break;
        default:
            unvoted += voter.weight;
            break;
        }
    }
    return QObject::tr("%1Y, %2N, %3A / %4 unvoted").arg(yes).arg(no).arg(abstain).arg(unvoted);
}

ProposalVoteDialog::ProposalVoteDialog(const QString& title, const QString& deadline, std::vector<ProposalVoter> voters,
                                       vote_outcome_enum_t outcome, QWidget* parent) :
    QDialog(parent),
    m_voters{std::move(voters)},
    m_table{new QTableWidget(this)},
    m_summary{new QLabel(this)},
    m_deadline{new QLabel(deadline, this)},
    m_outcome{new QComboBox(this)},
    m_submit{new QPushButton(tr("Vote %1").arg(ProposalVoteOutcome(outcome)), this)}
{
    setWindowTitle(tr("Proposal Votes"));
    resize(980, 480);
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(title, this);
    heading->setTextFormat(Qt::PlainText);
    heading->setWordWrap(true);
    layout->addWidget(heading);
    m_outcome->setObjectName("voteOutcome");
    for (const auto value : {VOTE_OUTCOME_YES, VOTE_OUTCOME_NO, VOTE_OUTCOME_ABSTAIN}) {
        m_outcome->addItem(ProposalVoteOutcome(value), static_cast<int>(value));
    }
    m_outcome->setCurrentIndex(m_outcome->findData(static_cast<int>(outcome)));
    connect(m_outcome, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { m_submit->setText(tr("Vote %1").arg(ProposalVoteOutcome(this->outcome()))); });
    layout->addWidget(m_outcome);
    m_deadline->setObjectName("dialogVotingDeadline");
    m_deadline->setWordWrap(true);
    layout->addWidget(m_deadline);
    auto* instructions = new QLabel(tr("Select the masternodes to vote with. Current funding votes are shown below. "
                                       "Changing a vote replaces that masternode's previous vote; the network limits "
                                       "updates to once per hour."),
                                    this);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    m_table->setObjectName("votingMasternodes");
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        {tr("Masternode"), tr("Voting Address"), tr("Weight"), tr("Current Vote"), tr("Vote Time"), tr("ProTx Hash")});
    m_table->setRowCount(static_cast<int>(m_voters.size()));
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto& voter = m_voters[row];
        auto* item = new QTableWidgetItem(voter.service);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        m_table->setItem(row, 0, item);
        m_table->setItem(row, 1, new QTableWidgetItem(voter.voting_address));
        m_table->setItem(row, 2, new QTableWidgetItem(QString::number(voter.weight)));
        m_table->setItem(row, 3, new QTableWidgetItem(ProposalVoteOutcome(voter.outcome)));
        m_table->setItem(row, 4,
                         new QTableWidgetItem(voter.time > 0
                                                  ? GUIUtil::dateTimeStr(QDateTime::fromSecsSinceEpoch(voter.time))
                                                  : QString{}));
        m_table->setItem(row, 5, new QTableWidgetItem(QString::fromStdString(voter.pro_tx_hash.ToString())));
    }
    layout->addWidget(m_table);
    auto* selection_buttons = new QHBoxLayout;
    for (const auto& [label, state] :
         {std::make_pair(tr("Select All"), Qt::Checked), std::make_pair(tr("Clear Selection"), Qt::Unchecked)}) {
        auto* button = new QPushButton(label, this);
        selection_buttons->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, state] {
            const QSignalBlocker blocker{m_table};
            for (int row = 0; row < m_table->rowCount(); ++row) {
                m_table->item(row, 0)->setCheckState(state);
            }
            updateSelection();
        });
    }
    selection_buttons->addStretch();
    layout->addLayout(selection_buttons);
    m_summary->setObjectName("selectionSummary");
    layout->addWidget(m_summary);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_submit->setObjectName("submitVote");
    buttons->addButton(m_submit, QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_table, &QTableWidget::itemChanged, this, &ProposalVoteDialog::updateSelection);
    layout->addWidget(buttons);
    updateSelection();
}

std::vector<uint256> ProposalVoteDialog::selectedMasternodes() const
{
    std::vector<uint256> selected;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0)->checkState() == Qt::Checked) selected.push_back(m_voters[row].pro_tx_hash);
    }
    return selected;
}

vote_outcome_enum_t ProposalVoteDialog::outcome() const
{
    return static_cast<vote_outcome_enum_t>(m_outcome->currentData().toInt());
}

void ProposalVoteDialog::setDeadline(const QString& deadline)
{
    m_deadline->setText(deadline);
}

void ProposalVoteDialog::updateSelection()
{
    int count{0}, weight{0};
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0)->checkState() == Qt::Checked) {
            ++count;
            weight += m_voters[row].weight;
        }
    }
    m_summary->setText(tr("Masternodes selected: %1 · Vote weight: %2").arg(count).arg(weight));
    m_submit->setEnabled(count > 0);
}
