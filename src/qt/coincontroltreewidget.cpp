// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/coincontroltreewidget.h>
#include <qt/coincontroldialog.h>

#include <QTreeWidgetItemIterator>

CoinControlTreeWidget::CoinControlTreeWidget(QWidget *parent) :
    QTreeWidget(parent)
{

}

void CoinControlTreeWidget::resetAnchor()
{
    m_lastClickedItem = nullptr;
}

void CoinControlTreeWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space) // press spacebar -> select checkbox
    {
        event->ignore();
        if (this->currentItem()) {
            int COLUMN_CHECKBOX = 0;
            this->currentItem()->setCheckState(COLUMN_CHECKBOX, ((this->currentItem()->checkState(COLUMN_CHECKBOX) == Qt::Checked) ? Qt::Unchecked : Qt::Checked));
        }
    }
    else if (event->key() == Qt::Key_Escape) // press esc -> close dialog
    {
        event->ignore();
        CoinControlDialog *coinControlDialog = static_cast<CoinControlDialog*>(this->parentWidget());
        coinControlDialog->done(QDialog::Accepted);
    }
    else
    {
        this->QTreeWidget::keyPressEvent(event);
    }
}

// Helper: check if an item is a leaf node (UTXO) by its 64-char tx hash
static bool isLeafItem(QTreeWidgetItem* item)
{
    int COLUMN_ADDRESS = 3;
    return item && item->data(COLUMN_ADDRESS, Qt::UserRole).toString().length() == 64;
}

void CoinControlTreeWidget::mouseReleaseEvent(QMouseEvent *event)
{
    int COLUMN_CHECKBOX = 0;

    QTreeWidgetItem* clickedItem = itemAt(event->pos());

    bool isShiftClick = (event->button() == Qt::LeftButton)
                        && (event->modifiers() & Qt::ShiftModifier)
                        && m_lastClickedItem
                        && clickedItem
                        && clickedItem != m_lastClickedItem
                        && isLeafItem(clickedItem);

    if (!isShiftClick) {
        // Normal click — let Qt handle the checkbox toggle on release
        QTreeWidget::mouseReleaseEvent(event);
        // Record anchor after toggle so we capture the post-toggle state
        if (clickedItem && isLeafItem(clickedItem)) {
            m_lastClickedItem = clickedItem;
        }
        return;
    }

    // Shift+click: select/deselect the range between anchor and target
    // Read the anchor's current check state live (not cached) so it stays
    // correct after bulk operations like Select All or parent tristate changes
    Qt::CheckState stateToApply = m_lastClickedItem->checkState(COLUMN_CHECKBOX);

    // Collect visible leaf items in display order, skipping children of
    // collapsed parent nodes so we don't toggle coins the user can't see
    std::vector<QTreeWidgetItem*> leafItems;
    int anchorIdx = -1;
    int targetIdx = -1;
    QTreeWidgetItemIterator it(this);
    while (*it) {
        if (isLeafItem(*it)) {
            QTreeWidgetItem* parent = (*it)->parent();
            if (!parent || parent->isExpanded()) {
                if (*it == m_lastClickedItem) anchorIdx = leafItems.size();
                if (*it == clickedItem) targetIdx = leafItems.size();
                leafItems.push_back(*it);
            }
        }
        ++it;
    }

    if (anchorIdx < 0 || targetIdx < 0) {
        QTreeWidget::mouseReleaseEvent(event);
        return;
    }

    if (anchorIdx > targetIdx) std::swap(anchorIdx, targetIdx);

    // Batch update: disable widget to suppress per-item updateLabels calls
    setEnabled(false);
    for (int i = anchorIdx; i <= targetIdx; ++i) {
        QTreeWidgetItem* item = leafItems[i];
        if (!item->isDisabled() && item->checkState(COLUMN_CHECKBOX) != stateToApply) {
            item->setCheckState(COLUMN_CHECKBOX, stateToApply);
        }
    }
    setEnabled(true);

    // Single label update for the whole batch
    CoinControlDialog* coinControlDialog = qobject_cast<CoinControlDialog*>(this->parentWidget());
    if (coinControlDialog) coinControlDialog->refreshLabels();
}
