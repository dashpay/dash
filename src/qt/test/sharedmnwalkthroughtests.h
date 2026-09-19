// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_SHAREDMNWALKTHROUGHTESTS_H
#define BITCOIN_QT_TEST_SHAREDMNWALKTHROUGHTESTS_H

#include <QObject>

namespace interfaces {
class Node;
} // namespace interfaces

//! End-to-end walkthrough of the shared masternode UI against a real in-process
//! node and three real wallets: the coordinator drafts a session and invites
//! two participants, all three rounds are exchanged through the clipboard, the
//! registration is broadcast and confirmed, and the resulting masternode is
//! then opened in the masternode list and in every maintenance dialog.
//!
//! When DASH_QT_SHOTS_DIR is set the walkthrough additionally writes a PNG of
//! every page and modal it visits into that directory plus a manifest.txt, so
//! the same code doubles as the screenshot generator for review evidence.
class SharedMnWalkthroughTests : public QObject
{
    Q_OBJECT

public:
    explicit SharedMnWalkthroughTests(interfaces::Node& node) :
        m_node(node)
    {
    }

private Q_SLOTS:
    void initTestCase();
    void walkthrough();

private:
    interfaces::Node& m_node;
};

#endif // BITCOIN_QT_TEST_SHAREDMNWALKTHROUGHTESTS_H
