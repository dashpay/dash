// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/proposalvotedialog.h>
#include <qt/test/proposalvotetests.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>

namespace {
std::vector<ProposalVoter> Voters()
{
    return {{uint256S("01"), "127.0.0.1:19999", "regular voting address", 1, VOTE_OUTCOME_YES, 1700000000},
            {uint256S("02"), "127.0.0.2:19999", "evo voting address", 4, VOTE_OUTCOME_NO, 1700000001},
            {uint256S("03"), "127.0.0.3:19999", "unvoted address", 1, VOTE_OUTCOME_NONE, 0}};
}
} // namespace

void ProposalVoteTests::selectionAndOutcome()
{
    ProposalVoteDialog dialog("<b>Untrusted proposal title</b>", "Deadline", Voters(), VOTE_OUTCOME_YES);
    auto* table = dialog.findChild<QTableWidget*>("votingMasternodes");
    auto* submit = dialog.findChild<QPushButton*>("submitVote");
    auto* summary = dialog.findChild<QLabel*>("selectionSummary");
    auto* outcome = dialog.findChild<QComboBox*>("voteOutcome");
    QVERIFY(table && submit && summary && outcome);
    QVERIFY(!submit->isEnabled());
    QVERIFY(dialog.selectedMasternodes().empty());
    QCOMPARE(table->item(0, 3)->text(), QString("Yes"));
    QCOMPARE(table->item(1, 3)->text(), QString("No"));
    QCOMPARE(table->item(2, 3)->text(), QString("Not voted"));
    QVERIFY(!table->item(0, 4)->text().isEmpty());
    QVERIFY(table->item(2, 4)->text().isEmpty());
    table->item(1, 0)->setCheckState(Qt::Checked);
    QVERIFY(submit->isEnabled());
    QVERIFY(dialog.selectedMasternodes() == std::vector<uint256>{uint256S("02")});
    QCOMPARE(summary->text(), QString("Masternodes selected: 1 · Vote weight: 4"));
    outcome->setCurrentIndex(outcome->findData(static_cast<int>(VOTE_OUTCOME_ABSTAIN)));
    QCOMPARE(dialog.outcome(), VOTE_OUTCOME_ABSTAIN);
    QCOMPARE(submit->text(), QString("Vote Abstain"));
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == "Select All") button->click();
    }
    QCOMPARE(dialog.selectedMasternodes().size(), size_t{3});
    QCOMPARE(summary->text(), QString("Masternodes selected: 3 · Vote weight: 6"));
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == "Clear Selection") button->click();
    }
    QVERIFY(dialog.selectedMasternodes().empty());
    QVERIFY(!submit->isEnabled());
    table->item(0, 0)->setCheckState(Qt::Checked);
    submit->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
}

void ProposalVoteTests::emptyWallet()
{
    ProposalVoteDialog dialog("Proposal", "Deadline", {}, VOTE_OUTCOME_NO);
    QVERIFY(dialog.selectedMasternodes().empty());
    QVERIFY(!dialog.findChild<QPushButton*>("submitVote")->isEnabled());
    dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
}

void ProposalVoteTests::weightedSummary()
{
    auto voters = Voters();
    QCOMPARE(ProposalVoteSummary(voters), QString("1Y, 4N, 0A / 1 unvoted"));
    voters[1].outcome = VOTE_OUTCOME_ABSTAIN;
    QCOMPARE(ProposalVoteSummary(voters), QString("1Y, 0N, 4A / 1 unvoted"));
    voters[2].outcome = VOTE_OUTCOME_YES;
    QCOMPARE(ProposalVoteSummary(voters), QString("2Y, 0N, 4A / 0 unvoted"));
    QCOMPARE(ProposalVoteSummary({}), QString("0Y, 0N, 0A / 0 unvoted"));
}

void ProposalVoteTests::deadline()
{
    QVERIFY(ProposalVotingDeadline(99, 100, 150).contains("1 blocks, block 100"));
    QVERIFY(ProposalVotingDeadline(99, 100, 150).contains("~"));
    QCOMPARE(ProposalVotingDeadline(100, 100, 150), QString("Voting deadline passed for this cycle (block 100)"));
    QCOMPARE(ProposalVotingDeadline(101, 100, 150), ProposalVotingDeadline(100, 100, 150));
    QVERIFY(ProposalVotingDeadline(100, 200, 150).contains("100 blocks, block 200"));
}
