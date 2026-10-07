// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/coincontroltreewidgettests.h>

#include <qt/coincontroltreewidget.h>

#include <QApplication>
#include <QHeaderView>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTreeWidgetItem>

#include <vector>

namespace {
constexpr int COLUMN_CHECKBOX{0};
constexpr int COLUMN_ADDRESS{3};
constexpr int COLUMN_COUNT{7};

//! A coin row, which CoinControlDialog marks by storing a 64-character txid in the address column
QTreeWidgetItem* AddCoin(CoinControlTreeWidget& tree, QTreeWidgetItem* group = nullptr)
{
    auto* coin{group ? new QTreeWidgetItem(group) : new QTreeWidgetItem(&tree)};
    coin->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    coin->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
    coin->setData(COLUMN_ADDRESS, Qt::UserRole, QString(64, '0'));
    coin->setText(COLUMN_ADDRESS, "address");
    return coin;
}

std::vector<QTreeWidgetItem*> AddCoins(CoinControlTreeWidget& tree, int count, QTreeWidgetItem* group = nullptr)
{
    std::vector<QTreeWidgetItem*> coins;
    coins.reserve(count);
    for (int i = 0; i < count; ++i) {
        coins.push_back(AddCoin(tree, group));
    }
    return coins;
}

//! An address row of the tree mode, which checks itself when all of its coins are checked
QTreeWidgetItem* AddGroup(CoinControlTreeWidget& tree)
{
    auto* group{new QTreeWidgetItem(&tree)};
    group->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    group->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
    return group;
}

void ShowTree(CoinControlTreeWidget& tree)
{
    tree.setColumnCount(COLUMN_COUNT);
    tree.resize(800, 600);
    tree.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tree));
}

//! Check states as a string, "X" for a checked coin and "." for an unchecked one
QString Marks(const std::vector<QTreeWidgetItem*>& coins)
{
    QString marks;
    for (const QTreeWidgetItem* coin : coins) {
        marks += coin->checkState(COLUMN_CHECKBOX) == Qt::Checked ? 'X' : '.';
    }
    return marks;
}

QPoint CheckboxPos(CoinControlTreeWidget& tree, QTreeWidgetItem* item)
{
    QRect cell{tree.visualItemRect(item)};
    cell.setRight(tree.header()->sectionViewportPosition(COLUMN_CHECKBOX) + tree.header()->sectionSize(COLUMN_CHECKBOX) - 1);
    QStyleOptionViewItem option;
    option.initFrom(&tree);
    option.rect = cell;
    option.features = QStyleOptionViewItem::HasCheckIndicator;
    return tree.style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, &tree).center();
}

void ClickCheckbox(CoinControlTreeWidget& tree, QTreeWidgetItem* item, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QTest::mouseClick(tree.viewport(), Qt::LeftButton, modifiers, CheckboxPos(tree, item));
}

void ShiftClickCheckbox(CoinControlTreeWidget& tree, QTreeWidgetItem* item)
{
    ClickCheckbox(tree, item, Qt::ShiftModifier);
}
} // namespace

void CoinControlTreeWidgetTests::init()
{
#if defined(Q_OS_MACOS)
    if (QApplication::platformName() == "minimal") {
        // Showing widgets with the minimal platform crashes on macOS (QTBUG-49686)
        QSKIP("Run with QT_QPA_PLATFORM=cocoa on macOS");
    }
#endif
}

void CoinControlTreeWidgetTests::shiftClickChecksRange()
{
    CoinControlTreeWidget tree;
    const auto coins{AddCoins(tree, 6)};
    ShowTree(tree);

    ClickCheckbox(tree, coins[1]);
    QCOMPARE(Marks(coins), QString(".X...."));
    ShiftClickCheckbox(tree, coins[4]);
    QCOMPARE(Marks(coins), QString(".XXXX."));
    // The range runs upwards from the anchor too
    ShiftClickCheckbox(tree, coins[0]);
    QCOMPARE(Marks(coins), QString("XXXXX."));
}

void CoinControlTreeWidgetTests::shiftClickUnchecksRange()
{
    CoinControlTreeWidget tree;
    const auto coins{AddCoins(tree, 6)};
    for (QTreeWidgetItem* coin : coins) {
        coin->setCheckState(COLUMN_CHECKBOX, Qt::Checked);
    }
    ShowTree(tree);

    // Unchecking a coin makes "unchecked" the state applied to the range
    ClickCheckbox(tree, coins[1]);
    ShiftClickCheckbox(tree, coins[4]);
    QCOMPARE(Marks(coins), QString("X....X"));
}

void CoinControlTreeWidgetTests::clicksOutsideCheckboxIgnored()
{
    CoinControlTreeWidget tree;
    const auto coins{AddCoins(tree, 6)};
    ShowTree(tree);
    const auto address_pos{[&](QTreeWidgetItem* coin) {
        return QPoint{tree.header()->sectionViewportPosition(COLUMN_ADDRESS) + 10, tree.visualItemRect(coin).center().y()};
    }};

    // Clicking a row outside the checkbox neither toggles the coin nor sets an anchor
    QTest::mouseClick(tree.viewport(), Qt::LeftButton, Qt::NoModifier, address_pos(coins[0]));
    ShiftClickCheckbox(tree, coins[3]);
    QCOMPARE(Marks(coins), QString("...X.."));

    // and a Shift-click outside the checkbox doesn't apply a range from the anchor
    QTest::mouseClick(tree.viewport(), Qt::LeftButton, Qt::ShiftModifier, address_pos(coins[5]));
    QCOMPARE(Marks(coins), QString("...X.."));
}

void CoinControlTreeWidgetTests::disabledCoinsSkipped()
{
    CoinControlTreeWidget tree;
    const auto coins{AddCoins(tree, 6)};
    // CoinControlDialog disables locked coins
    coins[2]->setFlags(coins[2]->flags().setFlag(Qt::ItemIsEnabled, false));
    ShowTree(tree);

    ClickCheckbox(tree, coins[0]);
    ShiftClickCheckbox(tree, coins[4]);
    QCOMPARE(Marks(coins), QString("XX.XX."));
}

void CoinControlTreeWidgetTests::collapsedGroupsSkipped()
{
    CoinControlTreeWidget tree;
    QTreeWidgetItem* group_a{AddGroup(tree)};
    const auto coins_a{AddCoins(tree, 2, group_a)};
    QTreeWidgetItem* group_b{AddGroup(tree)};
    const auto coins_b{AddCoins(tree, 2, group_b)};
    QTreeWidgetItem* group_c{AddGroup(tree)};
    const auto coins_c{AddCoins(tree, 2, group_c)};
    group_a->setExpanded(true);
    group_c->setExpanded(true);
    ShowTree(tree);

    ClickCheckbox(tree, coins_a[0]);
    ShiftClickCheckbox(tree, coins_c[1]);
    QCOMPARE(Marks(coins_a), QString("XX"));
    QCOMPARE(Marks(coins_b), QString(".."));
    QCOMPARE(Marks(coins_c), QString("XX"));
    QCOMPARE(group_a->checkState(COLUMN_CHECKBOX), Qt::Checked);
    QCOMPARE(group_b->checkState(COLUMN_CHECKBOX), Qt::Unchecked);
}

void CoinControlTreeWidgetTests::hiddenAnchorReplaced()
{
    CoinControlTreeWidget tree;
    QTreeWidgetItem* group_a{AddGroup(tree)};
    const auto coins_a{AddCoins(tree, 2, group_a)};
    QTreeWidgetItem* group_b{AddGroup(tree)};
    const auto coins_b{AddCoins(tree, 3, group_b)};
    group_a->setExpanded(true);
    group_b->setExpanded(true);
    ShowTree(tree);

    ClickCheckbox(tree, coins_a[0]);
    group_a->setExpanded(false);

    // With the anchor out of sight a Shift-click toggles just its coin, which becomes the new anchor
    ShiftClickCheckbox(tree, coins_b[0]);
    QCOMPARE(Marks(coins_b), QString("X.."));
    ShiftClickCheckbox(tree, coins_b[2]);
    QCOMPARE(Marks(coins_b), QString("XXX"));
    QCOMPARE(Marks(coins_a), QString("X."));
}

void CoinControlTreeWidgetTests::resetAnchorForgetsAnchor()
{
    CoinControlTreeWidget tree;
    const auto coins{AddCoins(tree, 6)};
    ShowTree(tree);

    ClickCheckbox(tree, coins[0]);
    // CoinControlDialog::updateView() resets the anchor when it rebuilds the tree
    tree.resetAnchor();
    ShiftClickCheckbox(tree, coins[3]);
    QCOMPARE(Marks(coins), QString("X..X.."));
}

void CoinControlTreeWidgetTests::rangeKeepsKeyboardFocus()
{
    CoinControlTreeWidget tree;
    const auto coins{AddCoins(tree, 4)};
    ShowTree(tree);
    tree.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&tree));

    ClickCheckbox(tree, coins[0]);
    QVERIFY(tree.hasFocus());
    // The range is applied with the tree briefly disabled, which must not leave Space toggling something else
    ShiftClickCheckbox(tree, coins[2]);
    QCOMPARE(Marks(coins), QString("XXX."));
    QVERIFY(tree.hasFocus());
}
