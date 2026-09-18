// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_MNSHARESESSIONTESTS_H
#define BITCOIN_QT_TEST_MNSHARESESSIONTESTS_H

#include <QObject>
#include <QTest>

class MnShareSessionTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void validateSharesMirror();
    void envelopeRoundTrip();
    void envelopeFingerprint();
    void draftRepliesMergeInAnyOrder();
    void lockedTermsAdoption();
    void penaltyPreviewMath();
    void earlyPeriodWording();
    void signatureVerification();
    void frozenEnvelopeMatchesFunding();
    void oversizedEnvelopesAreRefused();
    void parallelFundingSignatureMerge();
    void rpcMethodNamesAreRegistered();
};

#endif // BITCOIN_QT_TEST_MNSHARESESSIONTESTS_H
