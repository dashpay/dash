// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_VOTINGPAGE_H
#define BITCOIN_QT_PLATFORM_VOTINGPAGE_H

#include <platform/types.h>

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

class VotingFlow;

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
QT_END_NAMESPACE

/** The DashPay tab's username voting: the wallet's masternodes, the open
 *  contests with their votes, and a vote for a contender, to lock the name
 *  or to abstain, as one of those masternodes. */
class VotingPage : public QWidget
{
    Q_OBJECT

public:
    explicit VotingPage(QWidget* parent = nullptr);

    void setFlow(VotingFlow* flow) { m_flow = flow; }
    //! Show the flow's state; offer voting only when `writable`: no DashPay
    //! flow, this one included, is busy.
    void refresh(bool writable);

private:
    //! The label of the contest selected in the list; empty for none.
    std::string selectedLabel() const;
    //! The vote the choice box selects; nullopt for none.
    std::optional<platform::NameVote> selectedChoice() const;

    VotingFlow* m_flow{nullptr};
    QLabel* m_summary{nullptr};
    QPushButton* m_load{nullptr};
    QLabel* m_status{nullptr};
    QListWidget* m_contests{nullptr};
    QComboBox* m_masternode{nullptr};
    QComboBox* m_choice{nullptr};
    QPushButton* m_cast{nullptr};
    QLabel* m_rules{nullptr};
    //! The vote each m_choice entry after the first casts.
    std::vector<platform::NameVote> m_choices;
};

#endif // BITCOIN_QT_PLATFORM_VOTINGPAGE_H
