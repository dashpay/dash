// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_COINCONTROLTREEWIDGETTESTS_H
#define BITCOIN_QT_TEST_COINCONTROLTREEWIDGETTESTS_H

#include <QObject>

class CoinControlTreeWidgetTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void shiftClickChecksRange();
    void shiftClickUnchecksRange();
    void clicksOutsideCheckboxIgnored();
    void disabledCoinsSkipped();
    void collapsedGroupsSkipped();
    void hiddenAnchorReplaced();
    void resetAnchorForgetsAnchor();
    void rangeKeepsKeyboardFocus();
};

#endif // BITCOIN_QT_TEST_COINCONTROLTREEWIDGETTESTS_H
