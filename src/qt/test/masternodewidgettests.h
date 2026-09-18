// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_MASTERNODEWIDGETTESTS_H
#define BITCOIN_QT_TEST_MASTERNODEWIDGETTESTS_H

#include <QObject>

namespace interfaces {
class Node;
} // namespace interfaces

class MasternodeWidgetTests : public QObject
{
    Q_OBJECT

public:
    explicit MasternodeWidgetTests(interfaces::Node& node) :
        m_node(node)
    {
    }

private Q_SLOTS:
    void endpointTokenization();
    void providerNetInfoValidation_data();
    void providerNetInfoValidation();
    void operationRunnerThreading();
    void operationRunnerExceptions();
    void operationRunnerShutdownDelivery();
    void feeSourcePickerEligibility();
    void registeredCollateralExclusion();
    void wizardMinimumGeometry();
    void wizardPageValidation();
    void broadcastConfirmation();
    void masternodeListRegistrationAvailability();
    void sharedMasternodeOwnedFilter();
    void sharedMasternodeContextMenu();
    void standbyDissolutionDescription();
    void sharedMasternodeDetails();
    void wizardInteractionLifecycle();
    void registrationResultStates();
    void destinationValidation();
    void operatorKeyModes();

private:
    interfaces::Node& m_node;
};

#endif // BITCOIN_QT_TEST_MASTERNODEWIDGETTESTS_H
