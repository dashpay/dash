// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_PLATFORMTESTS_H
#define BITCOIN_QT_TEST_PLATFORMTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
} // namespace interfaces

//! DashPay GUI tests over a scripted platform::PlatformClient.
class PlatformTests : public QObject
{
public:
    explicit PlatformTests(interfaces::Node& node) :
        m_node(node)
    {
    }
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void optInGating();
    void routeSelection();
    void endpointsOffTheRouteAreNotPushed();
    void optInDisclosureCopy();
    void dashPayOptionsSection();
    void mainnetChainIdNotOverridable();
    void networkChangedAndInactive();
    void syncingNodePushesNoEndpoints();
    void networkResumePushesEndpointsAgain();
    void statusDescriptions();
    void staleBlockTimeDescription();
};

#endif // BITCOIN_QT_TEST_PLATFORMTESTS_H
