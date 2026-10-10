// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_PLATFORMTESTS_H
#define BITCOIN_QT_TEST_PLATFORMTESTS_H

#include <QObject>
#include <QTest>

class PlatformService;
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

    //! Push one evonode endpoint into the service's client, or none, as the
    //! node would after collecting them (the test chain has no evonodes).
    static void pushEndpoints(PlatformService& service, bool available);

    Q_OBJECT

private Q_SLOTS:
    void optInGating();
    void routeSelection();
    void endpointsOffTheRouteAreNotPushed();
    void optInDisclosureCopy();
    void dashPayOptionsSection();
    void inactiveNetworkAndSigning();
    void syncingNodePushesNoEndpoints();
    void networkResumePushesEndpointsAgain();
    void detachingClientModelStopsService();
    void gateClosedAfterStartIsShown();
    void onionProxyLaterStartsService();
    void statusDescriptions();
    void staleBlockTimeDescription();
    void avatarPaletteHasNoPurple();
    void registrationStepsAndReassurance();
    void registrationKeysAndContestedFunding();
    void registrationFundsOnlyWithoutExistingIdentity();
    void identityFlowConsensusSteering();
    void registrationAdoptsNameAlreadyOurs();
    void contestedNameNeedsIdentityCredits();
    void contestedDomainWaitsOnlyForOurContest();
    void documentStepWaitsForLaggingIdentityRead();
    void identityFlowOutageDoesNotFail();
    void identityFlowFreezesOnUnsupportedVersion();
    void layoutChangeKeepsUnconsumedFunding();
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
    void contactAcceptDecryptsAndImports();
    void contactSendRotatesVersionFromChain();
    void contactAcceptAfterOurRequestSendsNothing();
    void contactKeysFollowTheIdentityLayout();
    void contactSendAsksToUnlock();
    void contactAcceptAsksToUnlockOnce();
    void answeredRequestEstablishesContact();
    void contactResendUsesNewestRequest();
    void profileAndContactRequestShareTheNonce();
    void contactRequestWithoutIdentityReleasesTheNonce();
    void detachingKeepsOpenDialogUsable();
    void contactsPastTheCapAreNotComplete();
    void readsShownOnNewerProtocolVersion();
    void addContactMarksIdentitiesThatCannotReceive();
    void answeredRequestThatCannotFinishSaysWhy();
    void contactsPageKeepsSelectionAndClearsErrors();
    void contactsWaitForEndpointsOnResume();
    void contactRequestConfirmationIsNeverAFailure();
    void searchResultsAreNotPersisted();
    void contactMetadataShownWithoutRereadingRequests();
    void clearedContactNameReplacesSearchedName();
    void profilePublishConfirmedByProof();
    void profileDialogWaitsForTheCurrentProfile();
    void ignoredRequestsAreHiddenLocally();
    void dashboardHasNoSendDisableOrRefresh();
    void onlyOneFilledButton();
    void contactsRefreshFollowsChainLocks();
    void showRefreshThrottled();
    void firstIncomingRequestSelected();
    void addContactNeedsThreeCharacters();
};

#endif // BITCOIN_QT_TEST_PLATFORMTESTS_H
