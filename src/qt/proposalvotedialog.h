// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PROPOSALVOTEDIALOG_H
#define BITCOIN_QT_PROPOSALVOTEDIALOG_H

#include <governance/vote.h>
#include <uint256.h>

#include <QDialog>
#include <QString>

#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

struct ProposalVoter {
    uint256 pro_tx_hash;
    QString service;
    QString voting_address;
    int weight;
    vote_outcome_enum_t outcome;
    int64_t time;
};

QString ProposalVoteOutcome(vote_outcome_enum_t outcome);
QString ProposalVotingDeadline(int height, int deadline, int64_t spacing);
QString ProposalVoteSummary(const std::vector<ProposalVoter>& voters);

class ProposalVoteDialog : public QDialog
{
    Q_OBJECT

public:
    ProposalVoteDialog(const QString& title, const QString& deadline, std::vector<ProposalVoter> voters,
                       vote_outcome_enum_t outcome, QWidget* parent = nullptr);
    std::vector<uint256> selectedMasternodes() const;
    vote_outcome_enum_t outcome() const;
    void setDeadline(const QString& deadline);

private:
    void updateSelection();
    const std::vector<ProposalVoter> m_voters;
    QTableWidget* m_table;
    QLabel* m_summary;
    QLabel* m_deadline;
    QComboBox* m_outcome;
    QPushButton* m_submit;
};

#endif // BITCOIN_QT_PROPOSALVOTEDIALOG_H
