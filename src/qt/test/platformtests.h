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
    void chainIdStampedOnlyByVerifiedReads();
    void syncingNodePushesNoEndpoints();
    void networkResumePushesEndpointsAgain();
    void detachingClientModelStopsService();
    void gateClosedAfterStartIsShown();
    void statusDescriptions();
    void staleBlockTimeDescription();
    void avatarPaletteHasNoPurple();
    void registrationStepsAndReassurance();
    void registrationKeysAndContestedFunding();
    void identityFlowConsensusSteering();
    void registrationAdoptsNameAlreadyOurs();
    void contestedNameNeedsIdentityCredits();
    void identityFlowOutageDoesNotFail();
    void identityFlowFreezesOnUnsupportedVersion();
    void networkChangeDiscardsLoadedRecord();
    void networkChangeKeepsUnconsumedFunding();
    void identityCreateFromFundingLock();
    void identityFlowNeedsUnlock();
    void identityCreateNeedsUnlock();
    void registrationSignsOnceAndResumesAfterRestart();
    void abandonedFundingKeepsItsRecord();
    void fundingConflictEndsWhenFinal();
    void releasedFundingIsReleasedFromTheCard();
    void resumedFundingWaitsForVerifiedVersion();
    void frozenWritesReadNoProtocolVersion();
    void registrationSignsSpentStepAgain();
    void registrationSignsAgainAfterUpgrade();
    void storedFailuresAreWordedWhenShown();
    void usernameWizardEntry();
    void usernameProgressWording();
    void unconsumedFundingBlocksDisable();
    void balanceShownAsDash();
    void serviceStartsThroughTheProxy();
    void clientFailureIsShown();
};

#endif // BITCOIN_QT_TEST_PLATFORMTESTS_H
