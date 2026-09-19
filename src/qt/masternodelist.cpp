// Copyright (c) 2016-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/masternodelist.h>
#include <qt/forms/ui_masternodelist.h>

#include <core_io.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <key_io.h>
#include <primitives/transaction.h>
#include <script/standard.h>

#include <qt/clientfeeds.h>
#include <qt/clientmodel.h>
#include <qt/descriptiondialog.h>
#include <qt/guiutil.h>
#include <qt/masternodedialogs.h>
#include <qt/masternodewidgets.h>
#include <qt/masternodewizard.h>
#include <qt/protxsender.h>
#include <qt/sharedmncreatedialog.h>
#include <qt/sharedmndialogs.h>
#include <qt/sharedmnwidgets.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QEventLoop>
#include <QHeaderView>
#include <QMessageBox>
#include <QPointer>
#include <QMetaObject>
#include <QSettings>
#include <QSignalBlocker>
#include <QThread>

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

bool MasternodeListSortFilterProxyModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const
{
    // "Type" filter. Shared masternodes are MnType::Regular but surface as MasternodeModel::TYPE_SHARED
    // here, so the "Regular" filter shows only single-owner masternodes.
    if (m_type_filter != TypeFilter::All) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::TYPE, source_parent);
        int type = sourceModel()->data(idx, Qt::EditRole).toInt();
        if (m_type_filter == TypeFilter::Regular && type != static_cast<int>(MnType::Regular)) {
            return false;
        }
        if (m_type_filter == TypeFilter::Evo && type != static_cast<int>(MnType::Evo)) {
            return false;
        }
        if (m_type_filter == TypeFilter::Shared && type != MasternodeModel::TYPE_SHARED) {
            return false;
        }
    }

    // Banned filter
    if (m_hide_banned) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::STATUS, source_parent);
        int status_value = sourceModel()->data(idx, Qt::EditRole).toInt();
        if (status_value > 0) {
            return false;
        }
    }

    // Text-matching filter
    if (const auto& regex = filterRegularExpression(); !regex.pattern().isEmpty()) {
        QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);
        QString searchText = sourceModel()->data(idx, Qt::UserRole).toString();
        if (!searchText.contains(regex)) {
            return false;
        }
    }

    // "Owned" filter
    if (m_show_owned_only) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::PROTX_HASH, source_parent);
        QString proTxHash = sourceModel()->data(idx, Qt::DisplayRole).toString();
        if (!m_owned_mns.contains(proTxHash)) {
            return false;
        }
    }

    return true;
}

bool MasternodeListSortFilterProxyModel::lessThan(const QModelIndex& lhs, const QModelIndex& rhs) const
{
    if (lhs.column() == MasternodeModel::SERVICE) {
        QVariant lhs_data{sourceModel()->data(lhs, sortRole())};
        QVariant rhs_data{sourceModel()->data(rhs, sortRole())};
        if (lhs_data.userType() == QMetaType::QByteArray && rhs_data.userType() == QMetaType::QByteArray) {
            return lhs_data.toByteArray() < rhs_data.toByteArray();
        }
    }
    return QSortFilterProxyModel::lessThan(lhs, rhs);
}

MasternodeList::MasternodeList(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::MasternodeList),
    m_proxy_model(new MasternodeListSortFilterProxyModel(this)),
    m_model(new MasternodeModel(this))
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->label_count, ui->countLabel}, GUIUtil::FontWeight::Bold, 14);

    // Set up proxy model
    m_proxy_model->setSourceModel(m_model);
    m_proxy_model->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy_model->setSortRole(Qt::EditRole);

    // Set up table view
    ui->tableViewMasternodes->setModel(m_proxy_model);
    ui->tableViewMasternodes->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->tableViewMasternodes->verticalHeader()->setVisible(false);

    // Set column widths
    auto* header = ui->tableViewMasternodes->horizontalHeader();
    header->setStretchLastSection(false);
    for (int col = 0; col < MasternodeModel::COUNT; ++col) {
        if (col == MasternodeModel::SERVICE) {
            header->setSectionResizeMode(col, QHeaderView::Stretch);
        } else {
            header->setSectionResizeMode(col, QHeaderView::ResizeToContents);
        }
    }

    // Hide ProTx Hash column (used for internal lookup)
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::PROTX_HASH, true);

    ui->checkBoxOwned->setEnabled(false);

    contextMenuDIP3 = new QMenu(this);
    contextMenuDIP3->setToolTipsVisible(true);
    contextMenuDIP3->addAction(tr("Copy ProTx Hash"), this, &MasternodeList::copyProTxHash_clicked);
    contextMenuDIP3->addAction(tr("Copy Collateral Outpoint"), this, &MasternodeList::copyCollateralOutpoint_clicked);
    contextMenuDIP3->addSeparator();
    m_action_update_service = contextMenuDIP3->addAction(tr("Update Service…"), this, &MasternodeList::onUpdateService);
    m_action_update_registrar = contextMenuDIP3->addAction(tr("Update Registrar…"), this,
                                                           &MasternodeList::onUpdateRegistrar);
    m_action_update_share = contextMenuDIP3->addAction(tr("Change Reward Address…"), this, &MasternodeList::onUpdateShare);
    m_action_rotate_keys = contextMenuDIP3->addAction(tr("Rotate Keys…"), this, &MasternodeList::onRotateSharedKeys);
    m_action_dissolve = contextMenuDIP3->addAction(tr("Dissolve…"), this, &MasternodeList::onDissolve);
    m_action_standby = contextMenuDIP3->addAction(tr("Create Standby Dissolution…"), this,
                                                  &MasternodeList::onCreateStandbyDissolution);
    // Revoking ends the node's operator key on its own and cannot be undone, so
    // it is kept away from the routine actions above it
    contextMenuDIP3->addSeparator();
    m_action_revoke = contextMenuDIP3->addAction(tr("Revoke…"), this, &MasternodeList::onRevoke);
    contextMenuDIP3->addSeparator();

    QMenu* filterMenu = contextMenuDIP3->addMenu(tr("Filter by"));
    filterMenu->addAction(tr("Collateral Address"), this, &MasternodeList::filterByCollateralAddress);
    filterMenu->addAction(tr("Payout Address"), this, &MasternodeList::filterByPayoutAddress);
    m_action_filter_owner = filterMenu->addAction(tr("Owner Address"), this, &MasternodeList::filterByOwnerAddress);
    filterMenu->addAction(tr("Voting Address"), this, &MasternodeList::filterByVotingAddress);

    ui->btnRegisterMasternode->setEnabled(false);
    ui->btnRegisterMasternode->setToolTip(tr("Registering a masternode requires a wallet."));
    connect(ui->btnRegisterMasternode, &QPushButton::clicked, this, &MasternodeList::showRegisterWizard);
    ui->btnSharedMasternode->setEnabled(false);
    connect(ui->btnSharedMasternode, &QPushButton::clicked, this, &MasternodeList::showSharedMnCreateDialog);

    connect(ui->tableViewMasternodes, &QTableView::customContextMenuRequested, this, &MasternodeList::showContextMenuDIP3);
    connect(ui->tableViewMasternodes, &QTableView::doubleClicked, this, &MasternodeList::extraInfoDIP3_clicked);
    connect(m_proxy_model, &QSortFilterProxyModel::rowsInserted, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::rowsRemoved, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::modelReset, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::layoutChanged, this, &MasternodeList::updateFilteredCount);

    GUIUtil::updateFonts();

    // Load filter settings
    QSettings settings;
    ui->checkBoxHideBanned->setChecked(settings.value("mnListHideBanned", false).toBool());
    ui->comboBoxType->setCurrentIndex(settings.value("mnListTypeFilter", 0).toInt());
    ui->filterText->setText(settings.value("mnListFilterText", "").toString());
}

MasternodeList::~MasternodeList()
{
    delete ui;
}

void MasternodeList::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange) {
        QTimer::singleShot(0, m_model, &MasternodeModel::refreshIcons);
    }
}

void MasternodeList::setClientModel(ClientModel* model)
{
    if (clientModel != nullptr) {
        disconnect(clientModel, &ClientModel::numBlocksChanged, this, nullptr);
        disconnect(clientModel, &ClientModel::masternodeListChanged, this, nullptr);
    }
    this->clientModel = model;
    updateRegistrationAvailability();
    if (!clientModel) {
        return;
    }
    // Whether a masternode can be registered at all depends on the tip: a
    // wallet opened before v24 activates must not keep the shared masternode
    // button disabled until its models happen to be re-set
    connect(clientModel, &ClientModel::numBlocksChanged, this, &MasternodeList::updateRegistrationAvailability);
    connect(clientModel, &ClientModel::masternodeListChanged, this, &MasternodeList::updateRegistrationAvailability);
    m_feed = clientModel->feedMasternode();
    if (m_feed) {
        connect(m_feed, &MasternodeFeed::dataReady, this, &MasternodeList::updateMasternodeList);
        updateMasternodeList();
    }
}

void MasternodeList::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
    ui->checkBoxOwned->setEnabled(walletModel != nullptr);
    updateRegistrationAvailability();
    if (walletModel) {
        QSettings settings;
        ui->checkBoxOwned->setChecked(settings.value("mnListOwnedOnly", false).toBool());
    } else {
        const QSignalBlocker blocker{ui->checkBoxOwned};
        ui->checkBoxOwned->setChecked(false);
        m_proxy_model->setShowOwnedOnly(false);
        m_proxy_model->setMyMasternodeHashes({});
        m_proxy_model->forceInvalidateFilter();
        updateFilteredCount();
    }
}

void MasternodeList::updateRegistrationAvailability()
{
    const bool can_register{clientModel != nullptr && walletModel != nullptr &&
                            !walletModel->wallet().privateKeysDisabled()};
    ui->btnRegisterMasternode->setEnabled(can_register);
    if (walletModel == nullptr) {
        ui->btnRegisterMasternode->setToolTip(tr("Registering a masternode requires a wallet."));
    } else if (walletModel->wallet().privateKeysDisabled()) {
        ui->btnRegisterMasternode->setToolTip(
            walletModel->wallet().hasExternalSigner() ?
                tr("Masternode registration does not yet support external-signer wallets.") :
                tr("Masternode registration requires a wallet with private keys."));
    } else if (clientModel == nullptr) {
        ui->btnRegisterMasternode->setToolTip(tr("Masternode registration is unavailable until the node is ready."));
    } else {
        ui->btnRegisterMasternode->setToolTip(tr("Register a new masternode or EvoNode using this wallet"));
    }

    const bool v24_active{clientModel != nullptr && clientModel->node().isV24Active()};
    const bool can_shared{can_register && v24_active};
    ui->btnSharedMasternode->setEnabled(can_shared);
    if (walletModel == nullptr) {
        ui->btnSharedMasternode->setToolTip(tr("Managing shared masternodes requires a wallet."));
    } else if (walletModel->wallet().privateKeysDisabled()) {
        ui->btnSharedMasternode->setToolTip(tr("Managing shared masternodes requires a wallet with private keys."));
    } else if (clientModel == nullptr) {
        ui->btnSharedMasternode->setToolTip(tr("Shared masternode management is unavailable until the node is ready."));
    } else if (!v24_active) {
        ui->btnSharedMasternode->setToolTip(tr("Shared masternodes require the v24 hard fork, which is not active on this network yet."));
    } else {
        ui->btnSharedMasternode->setToolTip(tr("Create or continue a multi-party shared masternode registration session"));
    }
}

void MasternodeList::showRegisterWizard()
{
    if (!clientModel || !walletModel || walletModel->wallet().privateKeysDisabled()) return;
    RegisterMasternodeWizard dlg(clientModel->node(), walletModel, this);
    dlg.exec();
}

void MasternodeList::showSharedMnCreateDialog()
{
    if (!clientModel || !walletModel) {
        return;
    }
    SharedMnCreateDialog dlg(clientModel->node(), walletModel, this);
    dlg.exec();
}

const MasternodeEntry* MasternodeList::entryForProTxHash(const QString& pro_tx_hash) const
{
    if (m_model == nullptr) return nullptr;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const MasternodeEntry* entry{m_model->getEntryAt(m_model->index(row, 0))};
        if (entry != nullptr && entry->proTxHash().compare(pro_tx_hash, Qt::CaseInsensitive) == 0) return entry;
    }
    return nullptr;
}

QString MasternodeList::describeStandbyDissolution(const CMutableTransaction& tx, const MasternodeEntry* entry) const
{
    const auto ptx{GetTxPayload<CProDisTx>(tx)};
    if (!ptx) return tr("This transaction is not a readable shared masternode dissolution.");

    const BitcoinUnits::Unit unit{SharedMnDisplayUnit(walletModel)};
    const std::vector<interfaces::MnShare> shares{entry != nullptr ? entry->shares()
                                                                  : std::vector<interfaces::MnShare>{}};
    const auto share_label = [&shares](size_t index) {
        return tr("Share %1 of %2").arg(index + 1).arg(shares.size());
    };
    // An output paying somewhere no share asked for is the whole point of
    // showing this list, so it is named by its address rather than skipped
    const auto script_address = [](const CScript& script) {
        CTxDestination dest;
        if (!ExtractDestination(script, dest)) return tr("an unrecognised script");
        return QString::fromStdString(EncodeDestination(dest));
    };

    QStringList lines;
    lines << tr("Masternode: %1").arg(MasternodeWidgetUtil::chunked(QString::fromStdString(ptx->proTxHash.ToString())));
    if (entry == nullptr) {
        lines << tr("This masternode is not in the current masternode list.");
    } else {
        lines << tr("Service: %1").arg(entry->service());
        lines << tr("Collateral address: %1").arg(entry->collateralAddress());
    }
    if (ptx->actorIndex < shares.size()) {
        lines << tr("Fee paid by: %1").arg(share_label(ptx->actorIndex));
    }
    lines << (!shares.empty() && ptx->vchSigs.size() == shares.size() ? tr("Approved by: every share owner")
                                                                     : tr("Approved by: your signature only"));

    lines << QString{} << tr("Payouts:");
    for (const CTxOut& out : tx.vout) {
        const auto match = std::find_if(shares.begin(), shares.end(), [&out](const interfaces::MnShare& share) {
            return share.scriptRefund == out.scriptPubKey;
        });
        const QString payee{match != shares.end()
                                ? share_label(static_cast<size_t>(std::distance(shares.begin(), match)))
                                : script_address(out.scriptPubKey)};
        lines << QStringLiteral("  ") + tr("%1 → %2").arg(payee, SharedMnFormatAmount(unit, out.nValue));
    }

    const SharedMnDissolution dissolution{SharedMnReadDissolution(tx, ptx->actorIndex, shares)};
    if (!shares.empty()) {
        lines << tr("Fee: %1").arg(SharedMnFormatAmount(unit, dissolution.fee));
    }
    lines << QString{}
          << (dissolution.returnsPrincipal
                  ? tr("Every share's principal is returned to its own refund address. This cannot be undone.")
                  : tr("Note: this transaction does not return every share's full principal. This cannot be undone."));
    lines << QString{} << tr("Transaction: %1").arg(QString::fromStdString(CTransaction(tx).GetHash().ToString()));
    return lines.join(QLatin1Char('\n'));
}

void MasternodeList::broadcastStandbyDissolution(const QString& tx_hex)
{
    if (clientModel == nullptr) return;
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, tx_hex.trimmed().toStdString())) return;
    const QString txid{QString::fromStdString(CTransaction(tx).GetHash().ToString())};
    const auto ptx{GetTxPayload<CProDisTx>(tx)};
    if (QMessageBox::question(this, tr("Broadcast this standby dissolution?"),
                              describeStandbyDissolution(
                                  tx, ptx ? entryForProTxHash(QString::fromStdString(ptx->proTxHash.ToString()))
                                          : nullptr),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    // The list can be destroyed while the loop below runs (closing the wallet
    // deletes the owning WalletView), so the sender is not parented to it and
    // nothing touches `this` afterwards without checking it is still alive.
    ProTxSender sender(clientModel->node(), /*parent=*/nullptr);
    UniValue params(UniValue::VOBJ);
    params.pushKV("hexstring", tx_hex.trimmed().toStdString());
    ProTxResult result;
    QEventLoop loop;
    connect(&sender, &ProTxSender::finished, &loop, [&](const ProTxResult& r) {
        result = r;
        loop.quit();
    });
    if (!sender.execute(QStringLiteral("sendrawtransaction"), params, /*wallet_model=*/nullptr)) return;
    const QPointer<MasternodeList> self{this};
    setEnabled(false);
    loop.exec();
    if (self.isNull()) return;
    setEnabled(true);
    if (!result.ok) {
        QMessageBox::critical(this, tr("Broadcast failed"), result.message);
        return;
    }
    QMessageBox::information(this, tr("Standby dissolution sent"), tr("Transaction: %1").arg(txid));
}

void MasternodeList::openSharedMessage(const QString& text)
{
    if (clientModel == nullptr || walletModel == nullptr) return;
    const SharedMnImport::Detected detected{SharedMnImport::Detect(text)};
    switch (detected.kind) {
    case SharedMnImport::Kind::Session: {
        SharedMnCreateDialog dlg(clientModel->node(), walletModel, this);
        dlg.openSharedMessage(text);
        dlg.exec();
        return;
    }
    case SharedMnImport::Kind::Sigs: {
        const MasternodeEntry* entry{entryForProTxHash(detected.proTxHash)};
        if (entry == nullptr) {
            QMessageBox::warning(this, tr("Masternode not found"),
                                 tr("This message is about a masternode that is not in the list yet."));
            return;
        }
        if (detected.sigKind == QLatin1String("dissolve")) {
            DissolveDialog dlg(clientModel->node(), walletModel, *entry, m_model->currentHeight(), this);
            dlg.preloadEnvelope(text);
            dlg.exec();
        } else {
            RotateSharedKeysDialog dlg(clientModel->node(), walletModel, *entry, this);
            dlg.preloadEnvelope(text);
            dlg.exec();
        }
        return;
    }
    case SharedMnImport::Kind::StandbyHex:
        broadcastStandbyDissolution(text);
        return;
    case SharedMnImport::Kind::Unknown:
        QMessageBox::warning(this, tr("Nothing to open"), detected.error);
        return;
    }
}

void MasternodeList::showContextMenuDIP3(const QPoint& point)
{
    QModelIndex index = ui->tableViewMasternodes->indexAt(point);
    if (!index.isValid()) return;
    ui->tableViewMasternodes->setCurrentIndex(index);
    ui->tableViewMasternodes->selectRow(index.row());

    updateContextMenuActions(GetSelectedEntry());
    contextMenuDIP3->exec(QCursor::pos());
}

void MasternodeList::updateContextMenuActions(const MasternodeEntry* entry)
{
    const bool can_sign{walletModel != nullptr && !walletModel->wallet().privateKeysDisabled()};
    const bool is_shared{entry != nullptr && entry->isShared()};
    const bool owns_owner_key{entry != nullptr && can_sign &&
                              walletModel->wallet().isSpendable(PKHash(entry->keyIdOwnerRaw()))};
    const auto availability{MasternodeMaintenance::actionAvailability(can_sign, owns_owner_key)};

    m_action_update_service->setEnabled(availability.update_service);
    m_action_update_service->setToolTip(
        availability.update_service ? QString{} : tr("Requires a wallet capable of signing transactions"));
    // A shared masternode has no registrar to update on its own: its keys move
    // through a key rotation every share owner approves
    m_action_update_registrar->setVisible(!is_shared);
    m_action_update_registrar->setEnabled(availability.update_registrar);
    m_action_update_registrar->setToolTip(
        availability.update_registrar ? QString{} : tr("Requires this masternode's owner key in the wallet"));
    m_action_revoke->setEnabled(availability.revoke);
    m_action_revoke->setToolTip(availability.revoke ? QString{} : tr("Requires a wallet capable of signing transactions"));

    // Shared-only actions need one of this masternode's share owner keys in the wallet
    bool owns_share{false};
    if (can_sign && is_shared) {
        const auto& shares{entry->shares()};
        owns_share = std::any_of(shares.begin(), shares.end(), [&](const auto& share) {
            return walletModel->wallet().isSpendable(PKHash(share.keyIDOwner));
        });
    }
    const QString shared_tooltip{
        owns_share ? QString{} : tr("Requires one of this masternode's share owner keys in this wallet")};
    for (auto* action : {m_action_update_share, m_action_rotate_keys, m_action_dissolve, m_action_standby}) {
        action->setVisible(is_shared);
        action->setEnabled(owns_share);
        action->setToolTip(shared_tooltip);
    }
    if (owns_share) {
        const auto saved{MasternodeStandby::SavedDate(entry->proTxHash())};
        m_action_standby->setToolTip(saved.saved ? tr("Already saved on this computer on %1").arg(saved.date)
                                                 : tr("Not created on this computer yet"));
    }

    m_action_filter_owner->setToolTip(is_shared ? tr("Filters by one of this masternode's share owner addresses") :
                                                  QString{});
}

const MasternodeEntry* MasternodeList::selectedEntryForDialog()
{
    const auto* entry{GetSelectedEntry()};
    return entry != nullptr && clientModel != nullptr && walletModel != nullptr ? entry : nullptr;
}

void MasternodeList::onUpdateService()
{
    if (const auto* entry{selectedEntryForDialog()}) {
        UpdateServiceDialog dialog(clientModel->node(), walletModel, *entry, this);
        dialog.exec();
    }
}

void MasternodeList::onUpdateRegistrar()
{
    if (const auto* entry{selectedEntryForDialog()};
        entry != nullptr && walletModel->wallet().isSpendable(PKHash(entry->keyIdOwnerRaw()))) {
        UpdateRegistrarDialog dialog(clientModel->node(), walletModel, *entry, this);
        dialog.exec();
    }
}

void MasternodeList::onRevoke()
{
    if (const auto* entry{selectedEntryForDialog()}) {
        RevokeDialog dialog(clientModel->node(), walletModel, *entry, this);
        dialog.exec();
    }
}

void MasternodeList::onUpdateShare()
{
    const auto* entry = GetSelectedEntry();
    if (!entry || !clientModel || !walletModel || !entry->isShared()) {
        return;
    }
    UpdateShareDialog dlg(clientModel->node(), walletModel, *entry, this);
    dlg.exec();
}

void MasternodeList::onDissolve()
{
    openDissolveDialog(/*standby=*/false);
}

void MasternodeList::onCreateStandbyDissolution()
{
    openDissolveDialog(/*standby=*/true);
}

void MasternodeList::openDissolveDialog(bool standby)
{
    const auto* entry = GetSelectedEntry();
    if (!entry || !clientModel || !walletModel || !entry->isShared()) {
        return;
    }
    DissolveDialog dlg(clientModel->node(), walletModel, *entry, m_model->currentHeight(), this);
    if (standby) {
        dlg.selectStandbyTab();
    }
    dlg.exec();
}

void MasternodeList::onRotateSharedKeys()
{
    const auto* entry = GetSelectedEntry();
    if (!entry || !clientModel || !walletModel || !entry->isShared()) {
        return;
    }
    RotateSharedKeysDialog dlg(clientModel->node(), walletModel, *entry, this);
    dlg.exec();
}

void MasternodeList::updateMasternodeList()
{
    if (!clientModel || !m_feed) {
        return;
    }

    const auto feed = m_feed->data();
    if (!feed) {
        return;
    }

    if (!feed->m_valid) {
        qWarning() << "MasternodeList: fetch returned invalid data, scheduling retry";
        m_feed->requestRefresh();
        return;
    }

    MasternodeData ret;
    ret.m_list_height = feed->m_list_height;
    ret.m_entries = feed->m_entries;
    ret.m_valid = feed->m_valid;

    // If we don't have a wallet, nothing else to do...
    if (!walletModel) {
        setMasternodeList(std::move(ret), {}, {});
        return;
    }

    std::set<COutPoint> setOutpts;
    for (const auto& outpt : walletModel->wallet().listProTxCoins()) {
        setOutpts.emplace(outpt);
    }

    QSet<QString> owned_mns;
    QHash<QString, int> my_share_counts;
    for (const auto& entry : feed->m_entries) {
        if (isOwnedBy(walletModel->wallet(), setOutpts, *entry)) {
            owned_mns.insert(entry->proTxHash());
        }
        if (!entry->isShared()) continue;
        const auto& shares{entry->shares()};
        const auto count{std::count_if(shares.begin(), shares.end(), [&](const auto& share) {
            return walletModel->wallet().isSpendable(PKHash(share.keyIDOwner));
        })};
        if (count > 0) {
            my_share_counts.insert(entry->proTxHash(), static_cast<int>(count));
        }
    }
    setMasternodeList(std::move(ret), std::move(owned_mns), std::move(my_share_counts));
}

bool MasternodeList::isOwnedBy(interfaces::Wallet& wallet, const std::set<COutPoint>& protx_coins,
                               const MasternodeEntry& entry)
{
    if (protx_coins.count(entry.collateralOutpointRaw()) > 0) return true;
    if (wallet.isSpendable(PKHash(entry.keyIdOwnerRaw()))) return true;
    if (wallet.isSpendable(PKHash(entry.keyIdVotingRaw()))) return true;
    if (wallet.isSpendable(entry.scriptOperatorPayoutRaw())) return true;

    const auto script_payouts{entry.scriptPayoutsRaw()};
    if (std::any_of(script_payouts.begin(), script_payouts.end(),
                    [&](const auto& script) { return wallet.isSpendable(script); })) {
        return true;
    }

    // A shared masternode has a null keyIDOwner; its share owner keys take its place
    const auto share_owner_key_ids{entry.shareOwnerKeyIdsRaw()};
    if (std::any_of(share_owner_key_ids.begin(), share_owner_key_ids.end(),
                    [&](const auto& key_id) { return wallet.isSpendable(PKHash(key_id)); })) {
        return true;
    }

    // A participant whose rewards go to another wallet still owns the share
    // through its immutable refund destination, which is where the principal
    // returns on dissolution
    const auto share_refund_scripts{entry.shareRefundScriptsRaw()};
    return std::any_of(share_refund_scripts.begin(), share_refund_scripts.end(),
                       [&](const auto& script) { return wallet.isSpendable(script); });
}

void MasternodeList::setMasternodeList(MasternodeData&& list, QSet<QString>&& owned_mns,
                                       QHash<QString, int>&& my_share_counts)
{
    m_model->setCurrentHeight(list.m_list_height);
    m_model->setMyShareCounts(std::move(my_share_counts));
    m_model->reconcile(std::move(list.m_entries));

    if (walletModel) {
        m_proxy_model->setMyMasternodeHashes(std::move(owned_mns));
        if (ui->checkBoxOwned->isChecked()) {
            m_proxy_model->forceInvalidateFilter();
        }
    }

    updateFilteredCount();
}

void MasternodeList::updateFilteredCount()
{
    ui->countLabel->setText(QString::number(m_proxy_model->rowCount()));
}

void MasternodeList::on_filterText_textChanged(const QString& strFilterIn)
{
    m_proxy_model->setFilterRegularExpression(
        QRegularExpression(QRegularExpression::escape(strFilterIn), QRegularExpression::CaseInsensitiveOption));
    updateFilteredCount();

    QSettings settings;
    settings.setValue("mnListFilterText", strFilterIn);
}

void MasternodeList::on_comboBoxType_currentIndexChanged(int index)
{
    if (index < 0 || index >= static_cast<int>(MasternodeListSortFilterProxyModel::TypeFilter::COUNT)) {
        return;
    }
    const auto index_enum{static_cast<MasternodeListSortFilterProxyModel::TypeFilter>(index)};
    // The Type cell is redundant when the filter already names the type, except
    // for a shared masternode, where it is the only place the row says how many
    // of its shares this wallet holds
    ui->tableViewMasternodes->setColumnHidden(
        MasternodeModel::TYPE, index_enum != MasternodeListSortFilterProxyModel::TypeFilter::All &&
                                   index_enum != MasternodeListSortFilterProxyModel::TypeFilter::Shared);
    m_proxy_model->setTypeFilter(index_enum);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();

    QSettings settings;
    settings.setValue("mnListTypeFilter", index);
}

void MasternodeList::on_checkBoxOwned_stateChanged(int state)
{
    m_proxy_model->setShowOwnedOnly(state == Qt::Checked);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();

    QSettings settings;
    settings.setValue("mnListOwnedOnly", state == Qt::Checked);
}

void MasternodeList::on_checkBoxHideBanned_stateChanged(int state)
{
    const bool hide_banned{state == Qt::Checked};
    m_proxy_model->setHideBanned(hide_banned);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();

    QSettings settings;
    settings.setValue("mnListHideBanned", hide_banned);
}

const MasternodeEntry* MasternodeList::GetSelectedEntry()
{
    if (!m_model) {
        return nullptr;
    }

    QItemSelectionModel* selectionModel = ui->tableViewMasternodes->selectionModel();
    if (!selectionModel) {
        return nullptr;
    }

    QModelIndexList selected = selectionModel->selectedRows();
    if (selected.count() == 0) {
        return nullptr;
    }

    // Map from proxy to source model
    return m_model->getEntryAt(m_proxy_model->mapToSource(selected.at(0)));
}

void MasternodeList::extraInfoDIP3_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    QSet<int> my_share_indexes;
    if (walletModel && entry->isShared()) {
        const auto& shares{entry->shares()};
        for (size_t i = 0; i < shares.size(); ++i) {
            if (walletModel->wallet().isSpendable(PKHash(shares[i].keyIDOwner))) {
                my_share_indexes.insert(static_cast<int>(i));
            }
        }
    }
    auto* dialog = new DescriptionDialog(tr("Details for Masternode %1").arg(entry->proTxHash()),
                                         entry->toHtml(m_model->currentHeight(), my_share_indexes), /*parent=*/this);
    dialog->resize(1000, 500);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MasternodeList::copyProTxHash_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    QApplication::clipboard()->setText(entry->proTxHash());
}

void MasternodeList::copyCollateralOutpoint_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    QApplication::clipboard()->setText(entry->collateralOutpoint());
}

void MasternodeList::filterByCollateralAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->collateralAddress());
    }
}

void MasternodeList::filterByPayoutAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->payoutAddress());
    }
}

void MasternodeList::filterByOwnerAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->isShared() ? shareOwnerFilterAddress(*entry) : entry->ownerAddress());
    }
}

QString MasternodeList::shareOwnerFilterAddress(const MasternodeEntry& entry) const
{
    // A shared masternode has one owner address per share and the row matches
    // every one of them, so filtering by the share this wallet holds keeps the
    // masternode listed and finds the participant's other masternodes too
    const auto& shares{entry.shares()};
    if (shares.empty()) {
        return {};
    }
    const auto owned{std::find_if(shares.begin(), shares.end(), [&](const auto& share) {
        return walletModel != nullptr && walletModel->wallet().isSpendable(PKHash(share.keyIDOwner));
    })};
    const auto& share{owned != shares.end() ? *owned : shares.front()};
    return QString::fromStdString(EncodeDestination(PKHash(share.keyIDOwner)));
}

void MasternodeList::filterByVotingAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->votingAddress());
    }
}
