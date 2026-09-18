// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_MASTERNODEMAINTENANCETESTS_H
#define BITCOIN_QT_TEST_MASTERNODEMAINTENANCETESTS_H

#include <QObject>

namespace interfaces {
class Node;
}

class MasternodeMaintenanceTests : public QObject
{
    Q_OBJECT

public:
    explicit MasternodeMaintenanceTests(interfaces::Node& node) :
        m_node(node)
    {
    }

private Q_SLOTS:
    void automaticFeeSource();
    void operatorSecretValidation();
    void operatorPublicKeyEncoding();
    void actionRoleGating();
    void reconcileMutableFields();
    void serviceRequestConstruction();
    void registrarRequestConstruction();
    void revokeRequestConstruction();
    void dialogFieldGeometry();
    void dialogLifecycleAndSubmissionStates();
    void updateShareRewardValidation();
    void updateShareUnlocksTheWallet();
    void dissolveDialogTabsAndPayouts();
    void standbyDissolutionFile();
    void rotateOperatorKeyValidation();
    void rotationSenderComesFromTheInputs();
    void maintenanceEnvelopePreload();
    void dissolveRequestMustReturnPrincipal();

private:
    interfaces::Node& m_node;
};

#endif // BITCOIN_QT_TEST_MASTERNODEMAINTENANCETESTS_H
