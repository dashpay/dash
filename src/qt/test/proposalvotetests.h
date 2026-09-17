// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_PROPOSALVOTETESTS_H
#define BITCOIN_QT_TEST_PROPOSALVOTETESTS_H

#include <QObject>

class ProposalVoteTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void selectionAndOutcome();
    void emptyWallet();
    void weightedSummary();
    void deadline();
};

#endif // BITCOIN_QT_TEST_PROPOSALVOTETESTS_H
