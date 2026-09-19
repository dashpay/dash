// Copyright (c) 2016-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MASTERNODELIST_H
#define BITCOIN_QT_MASTERNODELIST_H

#include <qt/masternodemodel.h>

#include <QHash>
#include <QMenu>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <memory>
#include <set>

class ClientModel;
struct CMutableTransaction;
class MasternodeFeed;
class WalletModel;
struct MasternodeData;
namespace interfaces {
class MnList;
class Wallet;
using MnListPtr = std::shared_ptr<MnList>;
} // namespace interfaces
namespace Ui {
class MasternodeList;
} // namespace Ui

QT_BEGIN_NAMESPACE
class QModelIndex;
class QThread;
QT_END_NAMESPACE

class MasternodeListSortFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    enum class TypeFilter : uint8_t {
        All,
        Regular,
        Evo,
        Shared,
        COUNT
    };

    explicit MasternodeListSortFilterProxyModel(QObject* parent = nullptr) :
        QSortFilterProxyModel(parent) {}

    void forceInvalidateFilter() { invalidateFilter(); }
    void setHideBanned(bool hide) { m_hide_banned = hide; }
    void setMyMasternodeHashes(QSet<QString>&& hashes) { m_owned_mns = std::move(hashes); }
    void setShowOwnedOnly(bool show) { m_show_owned_only = show; }
    void setTypeFilter(TypeFilter type) { m_type_filter = type; }

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;
    bool lessThan(const QModelIndex& lhs, const QModelIndex& rhs) const override;

private:
    bool m_hide_banned{false};
    bool m_show_owned_only{false};
    QSet<QString> m_owned_mns;
    TypeFilter m_type_filter{TypeFilter::All};
};

/** Masternode Manager page widget */
class MasternodeList : public QWidget
{
    Q_OBJECT

    Ui::MasternodeList* ui;

    friend class MasternodeWidgetTests;

public:
    explicit MasternodeList(QWidget* parent = nullptr);
    ~MasternodeList() override;

    void setClientModel(ClientModel* clientModel);
    void setWalletModel(WalletModel* walletModel);

    //! Single entry point for every shared masternode message a user can
    //! paste, wherever they pasted it. The list is the only place that can
    //! resolve the proTxHash a maintenance message carries to the masternode
    //! it is about, so all three kinds are routed from here: a session
    //! envelope opens the creation wizard, a signing envelope opens the
    //! dissolution or key-rotation dialog for that masternode, and a standby
    //! dissolution is offered for broadcast.
    Q_INVOKABLE void openSharedMessage(const QString& text);

    //! Whether this wallet has a stake in `entry`: its collateral, one of its
    //! keys, or a destination it pays to. A shared masternode counts through
    //! any share's owner key or refund destination.
    static bool isOwnedBy(interfaces::Wallet& wallet, const std::set<COutPoint>& protx_coins,
                          const MasternodeEntry& entry);

protected:
    void changeEvent(QEvent* event) override;

private:
    ClientModel* clientModel{nullptr};
    MasternodeFeed* m_feed{nullptr};
    MasternodeListSortFilterProxyModel* m_proxy_model{nullptr};
    MasternodeModel* m_model{nullptr};
    QMenu* contextMenuDIP3{nullptr};
    QAction* m_action_update_service{nullptr};
    QAction* m_action_update_registrar{nullptr};
    QAction* m_action_revoke{nullptr};
    QAction* m_action_update_share{nullptr};
    QAction* m_action_dissolve{nullptr};
    QAction* m_action_rotate_keys{nullptr};
    QAction* m_action_standby{nullptr};
    QAction* m_action_filter_owner{nullptr};
    WalletModel* walletModel{nullptr};

    void setMasternodeList(MasternodeData&& data, QSet<QString>&& owned_mns, QHash<QString, int>&& my_share_counts);
    void updateRegistrationAvailability();
    //! Visibility, enabled state and tooltips of the context-menu actions for
    //! the entry the menu is about to open on
    void updateContextMenuActions(const MasternodeEntry* entry);
    void openDissolveDialog(bool standby);
    //! Entry for `pro_tx_hash` in the current list, or nullptr
    const MasternodeEntry* entryForProTxHash(const QString& pro_tx_hash) const;
    //! Confirm and send a standby dissolution somebody kept offline
    void broadcastStandbyDissolution(const QString& tx_hex);
    //! What `tx` does, for the confirmation before it is broadcast: which
    //! masternode it ends, which share acts, on whose approval, and what each
    //! output actually pays. `entry` is that masternode in the current list,
    //! or nullptr when it is not in it.
    QString describeStandbyDissolution(const CMutableTransaction& tx, const MasternodeEntry* entry) const;
    QString shareOwnerFilterAddress(const MasternodeEntry& entry) const;

    const MasternodeEntry* GetSelectedEntry();
    const MasternodeEntry* selectedEntryForDialog();

Q_SIGNALS:
    void doubleClicked(const QModelIndex&);

private Q_SLOTS:
    void copyCollateralOutpoint_clicked();
    void copyProTxHash_clicked();
    void showRegisterWizard();
    void showSharedMnCreateDialog();
    void extraInfoDIP3_clicked();
    void filterByCollateralAddress();
    void filterByOwnerAddress();
    void filterByPayoutAddress();
    void filterByVotingAddress();
    void on_checkBoxHideBanned_stateChanged(int state);
    void on_checkBoxOwned_stateChanged(int state);
    void on_comboBoxType_currentIndexChanged(int index);
    void on_filterText_textChanged(const QString& strFilterIn);
    void onCreateStandbyDissolution();
    void onDissolve();
    void onRevoke();
    void onRotateSharedKeys();
    void onUpdateRegistrar();
    void onUpdateService();
    void onUpdateShare();
    void showContextMenuDIP3(const QPoint&);
    void updateFilteredCount();
    void updateMasternodeList();
};

#endif // BITCOIN_QT_MASTERNODELIST_H
