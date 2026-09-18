// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sharedmndialogs.h>

#include <chainparams.h>
#include <coins.h>
#include <core_io.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <messagesigner.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/masternodemodel.h>
#include <qt/masternodewidgets.h>
#include <qt/mnsharesession.h>
#include <qt/protxsender.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sharedmnrpc.h>
#include <qt/sharedmnwidgets.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace {

constexpr const char* SIG_ENVELOPE_TYPE{"dash-shared-mn-sigs"};
constexpr int SIG_ENVELOPE_VERSION{1};
//! Duffs; mirrors the RPC-side default of "protx shared_dissolve" / "protx shared_dissolve_prepare"
constexpr CAmount DEFAULT_DISSOLVE_FEE{100000};
//! Duffs; the consensus floor is 1 duff and the ceiling CProDisTx::MAX_FEE, but
//! a dissolution that pays less than this never relays and one that pays more
//! only burns the actor's principal
constexpr CAmount MIN_DISSOLVE_FEE{1000};
constexpr CAmount MAX_DISSOLVE_FEE{1000000};
//! ~14 days of 2.5-minute blocks: below this distance to the penalty-free
//! boundary the unilateral tab suggests waiting instead
constexpr int NUDGE_BOUNDARY_BLOCKS{14 * 24 * 24};
constexpr qint64 SECONDS_PER_BLOCK{150};

//! "Share k of n" for the 0-based index the RPCs use
QString ShareOfLabel(int share_index, size_t share_count)
{
    return QCoreApplication::translate("SharedMnDialog", "Share %1 of %2")
        .arg(share_index + 1)
        .arg(share_count);
}

BitcoinAmountField* MakeFeeField(QWidget* parent, BitcoinUnits::Unit unit)
{
    auto* field = new BitcoinAmountField(parent);
    // BitcoinAmountField centres its editor inside whatever width a form row
    // hands it, which floats the fee far out of line with the rows above
    field->setMaximumWidth(300);
    field->setDisplayUnit(unit);
    field->SetMinValue(MIN_DISSOLVE_FEE);
    field->SetMaxValue(MAX_DISSOLVE_FEE);
    field->SetAllowEmpty(false);
    field->setValue(DEFAULT_DISSOLVE_FEE);
    return field;
}

//! The single canonical wording for "none of these shares can be acted on
//! from this wallet"; it is shown as a tooltip, an error and a status line
QString NoShareKeysMessage()
{
    return QCoreApplication::translate("SharedMnDialog",
                                       "This wallet does not hold any of this masternode's share owner keys.");
}

//! The single canonical wording for the activation gate
QString V24InactiveMessage()
{
    return QCoreApplication::translate(
        "SharedMnDialog", "Shared masternodes need the v24 upgrade, which is not active on this network yet.");
}

QString OwnerAddress(const interfaces::MnShare& share)
{
    return QString::fromStdString(EncodeDestination(PKHash(share.keyIDOwner)));
}

//! `address` shortened to something that fits beside a label. The full value
//! always travels with it as a tooltip.
QString ShortAddress(const QString& address)
{
    constexpr int HEAD{8};
    constexpr int TAIL{6};
    if (address.size() <= HEAD + TAIL + 1) return address;
    return address.left(HEAD) + QStringLiteral("…") + address.right(TAIL);
}

//! How a share is named where the chain is the only source: the wizard's
//! participant names are not registered anywhere, so "Share k of n" plus a
//! shortened owner address is the most anyone can be given.
QString ShareRowName(int share_index, size_t share_count, const interfaces::MnShare& share)
{
    return QCoreApplication::translate("SharedMnDialog", "%1 — %2")
        .arg(ShareOfLabel(share_index, share_count), ShortAddress(OwnerAddress(share)));
}

QString ScriptAddress(const CScript& script)
{
    CTxDestination dest;
    if (!ExtractDestination(script, dest)) return {};
    return QString::fromStdString(EncodeDestination(dest));
}

//! Say in the window, not only in a tooltip, why an action cannot be used
void ShowReason(QPushButton* button, QLabel* label, const QString& reason)
{
    button->setToolTip(reason);
    label->setText(reason);
    label->setVisible(!reason.isEmpty() && !button->isEnabled());
}

//! Header naming the masternode a dialog acts on. The 64-hex proTxHash is one
//! unbreakable word, so it is grouped into blocks that a label can wrap and
//! copied back unbroken.
QWidget* MakeProTxHeader(const QString& pro_tx_hash, QWidget* parent)
{
    return MasternodeWidgetUtil::makeCopyableValue(
        QCoreApplication::translate("SharedMnDialog", "Masternode: %1")
            .arg(MasternodeWidgetUtil::chunked(pro_tx_hash)),
        pro_tx_hash, parent);
}

//! Put `body` in a frameless scroll area so a dialog never clips its own
//! content when the share table or the approvals board grows
QScrollArea* MakeScroll(QWidget* body, QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("mnWizardScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // Never silently clip sideways: a bar the user can reach beats a column
    // that simply is not there
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->viewport()->setAutoFillBackground(false);
    body->setParent(scroll);
    scroll->setWidget(body);
    return scroll;
}

//! The "who gets what" table every dissolution screen shows: one row per
//! share, naming the address the principal returns to and the amount that
//! arrives there
QTableWidget* MakePayoutTable(QWidget* parent)
{
    auto* table = new QTableWidget(0, 3, parent);
    table->setHorizontalHeaderLabels({QCoreApplication::translate("SharedMnDialog", "Share"),
                                      QCoreApplication::translate("SharedMnDialog", "Refund address"),
                                      QCoreApplication::translate("SharedMnDialog", "Receives")});
    // What each participant receives is the point of this table; it must never
    // be elided, so its column is sized from its contents
    table->setTextElideMode(Qt::ElideNone);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setMinimumHeight(120);
    return table;
}

//! Fill row `row` of a MakePayoutTable(); `emphasise` marks the row the reader
//! is looking for, either their own share or the one paying the fee
void FillPayoutRow(QTableWidget* table, int row, const QString& share_text, const QString& address,
                   const QString& amount, bool emphasise)
{
    auto* share_item = new QTableWidgetItem(share_text);
    auto* refund_item = new QTableWidgetItem(address);
    auto* payout_item = new QTableWidgetItem(amount);
    if (emphasise) {
        QFont bold{share_item->font()};
        bold.setBold(true);
        share_item->setFont(bold);
        refund_item->setFont(bold);
        payout_item->setFont(bold);
    }
    table->setItem(row, 0, share_item);
    table->setItem(row, 1, refund_item);
    table->setItem(row, 2, payout_item);
}

void ShowStatusLabel(QLabel* label, const QString& message, bool error)
{
    label->setStyleSheet(GUIUtil::getThemedStyleQString(error ? GUIUtil::ThemedStyle::TS_ERROR
                                                              : GUIUtil::ThemedStyle::TS_SUCCESS));
    // The message can quote an envelope written by somebody else, or a node
    // error containing one; never markup
    label->setTextFormat(Qt::PlainText);
    label->setText(message);
    label->setVisible(!message.isEmpty());
}

//! Fill `combo` with the shares whose owner key `wallet_model` can sign with;
//! the item data is the share index
void FillMyShareCombo(QComboBox* combo, const std::vector<interfaces::MnShare>& shares, WalletModel* wallet_model)
{
    // Without a wallet model there is no spendability to test and nothing can
    // be submitted either, so list every share rather than an empty box
    const bool all{wallet_model == nullptr};
    if (!all && wallet_model->wallet().privateKeysDisabled()) return;
    const BitcoinUnits::Unit unit{SharedMnDisplayUnit(wallet_model)};
    for (size_t i = 0; i < shares.size(); ++i) {
        if (!all && !wallet_model->wallet().isSpendable(PKHash(shares[i].keyIDOwner))) continue;
        combo->addItem(QCoreApplication::translate("SharedMnDialog", "%1 — %2 — %3")
                           .arg(ShareOfLabel(static_cast<int>(i), shares.size()),
                                SharedMnFormatAmount(unit, shares[i].amount),
                                ShortAddress(OwnerAddress(shares[i]))),
                       static_cast<int>(i));
        combo->setItemData(combo->count() - 1, OwnerAddress(shares[i]), Qt::ToolTipRole);
    }
}

//! "block N (~date)" for a block `blocks_ahead` past the current tip
QString ApproxBlockDate(int blocks_ahead)
{
    const QDateTime when{QDateTime::currentDateTime().addSecs(static_cast<qint64>(blocks_ahead) * SECONDS_PER_BLOCK)};
    return QLocale().toString(when, QLocale::ShortFormat);
}

//! Recompute the digest the share owners of `pro_tx_hash` must sign for
//! `tx_hex`, mirroring the resolution in "protx shared_sign". A dissolution
//! that is not the principal-returning template is refused here rather than
//! given a digest to sign.
bool ComputeSharedSignHash(SharedSigCollector::Kind kind, const QString& pro_tx_hash,
                           const std::vector<interfaces::MnShare>& shares, const QString& tx_hex, uint256& hash,
                           QString& error)
{
    const size_t share_count{shares.size()};
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, tx_hex.toStdString())) {
        error = QCoreApplication::translate("SharedSigCollector", "The transaction is not valid transaction hex.");
        return false;
    }
    if (kind == SharedSigCollector::Kind::Dissolve) {
        if (tx.nType != TRANSACTION_PROVIDER_DISSOLVE) {
            error = QCoreApplication::translate("SharedSigCollector",
                                                "The transaction is not a dissolution transaction.");
            return false;
        }
        const auto opt_ptx = GetTxPayload<CProDisTx>(tx);
        if (!opt_ptx) {
            error = QCoreApplication::translate("SharedSigCollector", "The transaction payload is not deserializable.");
            return false;
        }
        if (QString::fromStdString(opt_ptx->proTxHash.ToString()).compare(pro_tx_hash, Qt::CaseInsensitive) != 0) {
            error = QCoreApplication::translate("SharedSigCollector",
                                                "The transaction dissolves a different masternode.");
            return false;
        }
        if (!opt_ptx->vchSigs.empty()) {
            error = QCoreApplication::translate("SharedSigCollector", "The transaction already carries signatures; "
                                                                      "expected the unsigned prepared transaction.");
            return false;
        }
        if (const auto dissolution{SharedMnReadDissolution(tx, opt_ptx->actorIndex, shares)};
            !dissolution.returnsPrincipal) {
            error = dissolution.error;
            return false;
        }
        // Unanimous mode: the digest commits to one signature per share
        hash = opt_ptx->MakeSignHash(CTransaction(tx), static_cast<uint8_t>(share_count));
        return true;
    }
    if (tx.nType != TRANSACTION_PROVIDER_UPDATE_SHARED_REGISTRAR) {
        error = QCoreApplication::translate("SharedSigCollector", "The transaction is not a shared registrar update.");
        return false;
    }
    const auto opt_ptx = GetTxPayload<CProUpSharedRegTx>(tx);
    if (!opt_ptx) {
        error = QCoreApplication::translate("SharedSigCollector", "The transaction payload is not deserializable.");
        return false;
    }
    if (QString::fromStdString(opt_ptx->proTxHash.ToString()).compare(pro_tx_hash, Qt::CaseInsensitive) != 0) {
        error = QCoreApplication::translate("SharedSigCollector", "The transaction updates a different masternode.");
        return false;
    }
    // SER_GETHASH serialization excludes the signature vector
    hash = ::SerializeHash(*opt_ptx);
    return true;
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// UpdateShareDialog
// ----------------------------------------------------------------------------

UpdateShareDialog::UpdateShareDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent) :
    MasternodeActionDialog(node, wallet_model, entry, parent),
    m_v24_active{node.isV24Active()},
    m_is_shared{entry.isShared()},
    m_voting_address{entry.votingAddress()},
    m_shares{entry.shares()},
    m_sender{new ProTxSender(node, this)}
{
    connect(m_sender, &ProTxSender::finished, this, [this](const ProTxResult& res) {
        setBusy(false);
        if (res.ok) {
            accept();
        } else {
            showError(res.message);
        }
    });
    auto* form = new QFormLayout();

    m_share_combo = new QComboBox(this);
    m_share_combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    FillMyShareCombo(m_share_combo, m_shares, wallet_model);
    connect(m_share_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, &UpdateShareDialog::validate);
    form->addRow(tr("Your share:"), m_share_combo);

    auto* reward_row = new QWidget(this);
    auto* reward_layout = new QHBoxLayout(reward_row);
    reward_layout->setContentsMargins(0, 0, 0, 0);
    m_reward_edit = new QValidatedLineEdit(reward_row);
    GUIUtil::setupAddressWidget(m_reward_edit, this);
    m_reward_edit->setPlaceholderText(tr("Address that receives this share's rewards"));
    connect(m_reward_edit, &QLineEdit::textChanged, this, &UpdateShareDialog::validate);
    reward_layout->addWidget(m_reward_edit, /*stretch=*/1);
    m_use_new_button = new QPushButton(tr("Use new address"), reward_row);
    connect(m_use_new_button, &QPushButton::clicked, this, &UpdateShareDialog::useNewAddress);
    reward_layout->addWidget(m_use_new_button);
    m_use_refund_button = new QPushButton(tr("Use refund address"), reward_row);
    connect(m_use_refund_button, &QPushButton::clicked, this, &UpdateShareDialog::useRefundAddress);
    reward_layout->addWidget(m_use_refund_button);
    for (QPushButton* const button : {m_use_new_button, m_use_refund_button}) {
        SharedMnMakeSecondary(button);
    }
    form->addRow(tr("New reward address:"), reward_row);

    // The RPC takes no automatic fee source: it funds the update from one named
    // address of this wallet
    m_fee_source = addFeeSourceRow(form, tr("The fee is paid from this address of your wallet."));
    m_fee_source->setAutomaticOption(QString{});
    connect(m_fee_source, qOverload<int>(&QComboBox::currentIndexChanged), this, &UpdateShareDialog::validate);

    setupUi(tr("Change Reward Address"),
            tr("Change where your share of this masternode's rewards is paid. Only your share is affected; the "
               "share's amount and refund address never change."),
            form, tr("Send Update"));
    // Shares, addresses and balances are long, and a wrapped hint must not be
    // painted over the row below it: size the window from what it has to show
    SharedMnFitWrappedLabels(this);
    SharedMnSizeFromContent(this, std::max(minimumWidth(), sizeHint().width()));

    if (!m_is_shared) {
        showError(tr("This masternode is not shared and has no share table."));
    } else if (!m_v24_active) {
        showError(V24InactiveMessage());
    } else if (m_share_combo->count() == 0) {
        showError(NoShareKeysMessage());
    }
    validate();
}

QString UpdateShareDialog::RewardAddressProblem(const QString& address, const QString& voting_address,
                                                const std::vector<interfaces::MnShare>& shares)
{
    const QString trimmed{address.trimmed()};
    if (trimmed.isEmpty()) {
        // "protx shared_update_share" has no "keep the current one" input: a
        // share that should pay to its refund address again has to name it
        return QCoreApplication::translate("UpdateShareDialog", "Enter the address that should receive the rewards.");
    }
    if (!IsValidDestination(DecodeDestination(trimmed.toStdString()))) {
        return QCoreApplication::translate("UpdateShareDialog", "Enter a valid Dash address.");
    }
    if (trimmed == voting_address) {
        return QCoreApplication::translate("UpdateShareDialog",
                                           "The reward address must differ from the voting address and every owner "
                                           "address.");
    }
    for (const auto& share : shares) {
        if (trimmed == OwnerAddress(share)) {
            return QCoreApplication::translate("UpdateShareDialog",
                                               "The reward address must differ from the voting address and every "
                                               "owner address.");
        }
    }
    return {};
}

void UpdateShareDialog::validate()
{
    const bool has_share{m_share_combo->currentIndex() >= 0};
    m_use_refund_button->setEnabled(has_share);
    m_use_new_button->setEnabled(canSign());

    const QString problem{RewardAddressProblem(m_reward_edit->text(), m_voting_address, m_shares)};
    m_reward_edit->setValid(problem.isEmpty() || m_reward_edit->text().trimmed().isEmpty());

    if (!m_is_shared) {
        showError(tr("This masternode is not shared and has no share table."));
    } else if (!m_v24_active) {
        showError(V24InactiveMessage());
    } else if (m_share_combo->count() == 0) {
        showError(NoShareKeysMessage());
    } else if (!m_reward_edit->text().trimmed().isEmpty() && !problem.isEmpty()) {
        showError(problem);
    } else if (m_fee_source->selectedAddress().isEmpty()) {
        showError(tr("Choose an address of this wallet to pay the fee from."));
    } else {
        clearError();
    }

    setOkValid(m_v24_active && m_is_shared && has_share && problem.isEmpty() &&
               !m_fee_source->selectedAddress().isEmpty());
}

void UpdateShareDialog::useRefundAddress()
{
    const int share_index{m_share_combo->currentData().toInt()};
    if (m_share_combo->currentIndex() < 0 || share_index < 0 ||
        static_cast<size_t>(share_index) >= m_shares.size()) {
        return;
    }
    m_reward_edit->setText(ScriptAddress(m_shares[share_index].scriptRefund));
}

void UpdateShareDialog::useNewAddress()
{
    if (m_wallet_model == nullptr) {
        showError(tr("No wallet is available."));
        return;
    }
    auto dest{m_wallet_model->wallet().getNewDestination(/*label=*/"")};
    if (!dest) {
        showError(tr("Could not generate a new address: %1")
                      .arg(QString::fromStdString(util::ErrorString(dest).translated)));
        return;
    }
    m_reward_edit->setText(QString::fromStdString(EncodeDestination(*dest)));
}

void UpdateShareDialog::submit()
{
    if (m_share_combo->currentIndex() < 0) return;
    // "protx shared_update_share" signs with the share owner key, so an
    // encrypted wallet has to be unlocked before the call rather than told off
    // by the node afterwards
    if (!ensureUnlocked()) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", QString::fromStdString(m_protx_hash.ToString()).toStdString());
    params.pushKV("shareIndex", m_share_combo->currentData().toInt());
    params.pushKV("rewardAddress", m_reward_edit->text().trimmed().toStdString());
    params.pushKV("feeSourceAddress", m_fee_source->selectedAddress().toStdString());

    clearError();
    setBusy(true);
    m_sender->execute(shared_mn_rpc::UPDATE_SHARE, params, m_wallet_model);
}

// ----------------------------------------------------------------------------
// SharedSigCollector
// ----------------------------------------------------------------------------

SharedSigCollector::SharedSigCollector(Kind kind, const QString& pro_tx_hash,
                                       const std::vector<interfaces::MnShare>& shares, WalletModel* wallet_model,
                                       QWidget* parent) :
    QWidget(parent),
    m_kind{kind},
    m_protx_hash{pro_tx_hash},
    m_shares{shares},
    m_wallet_model{wallet_model}
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);

    m_board = new SharedMnStatusBoard(this);
    m_board->setDisplayUnit(SharedMnDisplayUnit(m_wallet_model));
    m_board->setColumns({tr("Approved")});
    std::vector<SharedMnStatusBoard::Row> rows;
    rows.reserve(m_shares.size());
    int you_row{-1};
    for (size_t i = 0; i < m_shares.size(); ++i) {
        rows.push_back({ShareRowName(static_cast<int>(i), m_shares.size(), m_shares[i]), m_shares[i].amount,
                        OwnerAddress(m_shares[i])});
        if (you_row < 0 && m_wallet_model != nullptr && !m_wallet_model->wallet().privateKeysDisabled() &&
            m_wallet_model->wallet().isSpendable(PKHash(m_shares[i].keyIDOwner))) {
            you_row = static_cast<int>(i);
        }
    }
    m_board->setShares(rows);
    m_board->setYouRow(you_row);
    layout->addWidget(m_board);

    auto* buttons = new QHBoxLayout();
    m_copy_button = new QPushButton(tr("Copy Request"), this);
    connect(m_copy_button, &QPushButton::clicked, this, &SharedSigCollector::copyEnvelope);
    buttons->addWidget(m_copy_button);
    m_save_button = new QPushButton(tr("Save Request…"), this);
    connect(m_save_button, &QPushButton::clicked, this, &SharedSigCollector::saveEnvelope);
    buttons->addWidget(m_save_button);
    auto* paste_button = new QPushButton(tr("Paste"), this);
    connect(paste_button, &QPushButton::clicked, this, &SharedSigCollector::pasteEnvelope);
    buttons->addWidget(paste_button);
    auto* load_button = new QPushButton(tr("Open File…"), this);
    connect(load_button, &QPushButton::clicked, this, &SharedSigCollector::loadEnvelope);
    buttons->addWidget(load_button);
    // Handing the request out is the action of this block; saving it, pasting a
    // reply and opening a file are alternative routes to the same place
    for (QPushButton* const button : {m_save_button, paste_button, load_button}) {
        SharedMnMakeSecondary(button);
    }
    buttons->addStretch();
    layout->addLayout(buttons);

    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    layout->addWidget(m_status_label);

    refreshUi();
}

QString SharedSigCollector::code() const
{
    return shared_mn::EnvelopeFingerprint(envelopeJson());
}

QString SharedSigCollector::signHashHex() const
{
    if (m_tx_hex.isEmpty()) return {};
    uint256 hash;
    QString error;
    if (!ComputeSharedSignHash(m_kind, m_protx_hash, m_shares, m_tx_hex, hash, error)) return {};
    return QString::fromStdString(hash.ToString());
}

bool SharedSigCollector::setTransaction(const QString& tx_hex, QString& error)
{
    const QString trimmed{tx_hex.trimmed()};
    uint256 hash;
    if (!ComputeSharedSignHash(m_kind, m_protx_hash, m_shares, trimmed, hash, error)) {
        return false;
    }
    if (!m_tx_hex.isEmpty() && m_tx_hex != trimmed) {
        error = tr("This message is about a different transaction than the one already here. One of the two copies "
                   "is stale; keep the current one and ask for a fresh reply.");
        return false;
    }
    if (m_tx_hex == trimmed) return true;
    m_tx_hex = trimmed;
    refreshUi();
    Q_EMIT changed();
    return true;
}

int SharedSigCollector::addSignatures(const UniValue& entries, QString& error)
{
    QStringList problems;
    if (m_tx_hex.isEmpty()) {
        error = tr("Paste the request itself before pasting approvals.");
        return 0;
    }
    uint256 sign_hash;
    if (QString hash_err; !ComputeSharedSignHash(m_kind, m_protx_hash, m_shares, m_tx_hex, sign_hash, hash_err)) {
        error = hash_err;
        return 0;
    }
    if (!entries.isArray()) {
        error = tr("This message does not contain any approvals.");
        return 0;
    }

    int absorbed{0};
    for (const UniValue& entry : entries.getValues()) {
        if (!entry.isObject() || !entry.exists("shareIndex") || !entry.exists("signature")) {
            problems << tr("An approval is missing its share number or its signature.");
            continue;
        }
        int index{-1};
        try {
            index = entry.find_value("shareIndex").getInt<int>();
        } catch (const std::exception&) {
            problems << tr("An approval has an unreadable share number.");
            continue;
        }
        if (index < 0 || static_cast<size_t>(index) >= m_shares.size()) {
            problems << tr("Share number %1 is out of range.").arg(index + 1);
            continue;
        }
        const QString sig_b64{QString::fromStdString(entry.find_value("signature").isStr() ? entry.find_value("signature").get_str() : std::string{})};
        const auto sig{DecodeBase64(sig_b64.toStdString())};
        if (!sig || sig->size() != 65) {
            problems << tr("%1: the signature is not a base64 65-byte compact signature.")
                            .arg(ShareOfLabel(index, m_shares.size()));
            continue;
        }
        if (std::string str_error; !CHashSigner::VerifyHashCanonical(sign_hash, m_shares[index].keyIDOwner, *sig, str_error)) {
            problems << tr("%1's approval does not match the prepared transaction. Ask %2 to paste the current request and approve again.")
                            .arg(ShareOfLabel(index, m_shares.size()), OwnerAddress(m_shares[index]));
            continue;
        }
        const auto it = m_sigs.find(index);
        if (it != m_sigs.end()) {
            if (DecodeBase64(it->second.toStdString()) == sig) continue; // byte-identical duplicate
            problems << tr("%1 already has an approval from another reply.").arg(ShareOfLabel(index, m_shares.size()));
            continue;
        }
        m_sigs.emplace(index, sig_b64);
        ++absorbed;
    }
    error = problems.join(QLatin1Char('\n'));
    if (absorbed > 0) {
        refreshUi();
        Q_EMIT changed();
    }
    return absorbed;
}

UniValue SharedSigCollector::signaturesArray() const
{
    UniValue arr(UniValue::VARR);
    for (const auto& [index, sig] : m_sigs) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("shareIndex", index);
        entry.pushKV("signature", sig.toStdString());
        arr.push_back(entry);
    }
    return arr;
}

UniValue SharedSigCollector::envelopeJson() const
{
    UniValue json(UniValue::VOBJ);
    json.pushKV("type", SIG_ENVELOPE_TYPE);
    json.pushKV("version", SIG_ENVELOPE_VERSION);
    json.pushKV("network", Params().NetworkIDString());
    json.pushKV("kind", m_kind == Kind::Dissolve ? "dissolve" : "registrar");
    json.pushKV("proTxHash", m_protx_hash.toStdString());
    json.pushKV("tx", m_tx_hex.toStdString());
    json.pushKV("signatures", signaturesArray());
    shared_mn::AppendFingerprint(json);
    return json;
}

bool SharedSigCollector::importEnvelope(const QString& text, QString& error)
{
    UniValue json;
    if (!json.read(text.trimmed().toStdString())) {
        error = tr("This is not a shared masternode message.");
        return false;
    }
    try {
        return importEnvelopeChecked(json, error);
    } catch (const std::exception&) {
        // UniValue accessors throw on unexpectedly typed fields
        error = tr("This is not a shared masternode message.");
        return false;
    }
}

bool SharedSigCollector::importEnvelopeChecked(const UniValue& json, QString& error)
{
    // A bare array is accepted as signature entries for the already-adopted transaction
    if (json.isArray()) {
        QString sig_error;
        const int absorbed{addSignatures(json, sig_error)};
        error = sig_error;
        return absorbed > 0 || sig_error.isEmpty();
    }
    if (!json.isObject() || !json.exists("type") || json.find_value("type").get_str() != SIG_ENVELOPE_TYPE) {
        error = tr("This is not a shared masternode message.");
        return false;
    }
    QString fingerprint_warning;
    shared_mn::CheckFingerprint(json, fingerprint_warning);
    const std::string our_network{Params().NetworkIDString()};
    const std::string their_network{json.exists("network") ? json.find_value("network").get_str() : std::string{}};
    if (their_network != our_network) {
        error = tr("This message is for the %1 network; this wallet runs on %2.")
                    .arg(QString::fromStdString(their_network), QString::fromStdString(our_network));
        return false;
    }
    const std::string expected_kind{m_kind == Kind::Dissolve ? "dissolve" : "registrar"};
    if (!json.exists("kind") || json.find_value("kind").get_str() != expected_kind) {
        error = tr("This message is about a different kind of change to this masternode.");
        return false;
    }
    if (!json.exists("proTxHash") ||
        QString::fromStdString(json.find_value("proTxHash").get_str()).compare(m_protx_hash, Qt::CaseInsensitive) != 0) {
        error = tr("This message is about a different masternode.");
        return false;
    }
    if (json.exists("tx") && !json.find_value("tx").get_str().empty()) {
        if (!setTransaction(QString::fromStdString(json.find_value("tx").get_str()), error)) {
            return false;
        }
    }
    if (json.exists("signatures")) {
        QString sig_error;
        addSignatures(json.find_value("signatures"), sig_error);
        error = sig_error;
    }
    if (!fingerprint_warning.isEmpty()) {
        error = error.isEmpty() ? fingerprint_warning
                                : fingerprint_warning + QLatin1Char('\n') + error;
    }
    return true;
}

void SharedSigCollector::copyEnvelope()
{
    const UniValue json{envelopeJson()};
    GUIUtil::setClipboard(QString::fromStdString(json.write(/*prettyIndent=*/2)));
    showCodeLine(tr("Copied · Code %1").arg(shared_mn::EnvelopeFingerprint(json)));
    showStatus(QString{}, /*error=*/false);
}

void SharedSigCollector::saveEnvelope()
{
    const QString filename{GUIUtil::getSaveFileName(this, tr("Save Request"), QString{},
                                                    tr("Shared masternode message (*.json)"), nullptr)};
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        showStatus(tr("Could not write to %1.").arg(filename), /*error=*/true);
        return;
    }
    QTextStream out(&file);
    const UniValue json{envelopeJson()};
    out << QString::fromStdString(json.write(/*prettyIndent=*/2));
    file.close();
    showCodeLine(tr("Saved · Code %1").arg(shared_mn::EnvelopeFingerprint(json)));
    showStatus(tr("Saved to %1.").arg(filename), /*error=*/false);
}

void SharedSigCollector::pasteEnvelope()
{
    const QString text{QApplication::clipboard()->text()};
    // Same cap as loadEnvelope(): a request is a few kilobytes and parsing
    // happens on the GUI thread
    if (text.toUtf8().size() > MAX_ENVELOPE_FILE_BYTES) {
        showStatus(tr("That message is too large to be a shared masternode message."), /*error=*/true);
        return;
    }
    importAndReport(text);
}

void SharedSigCollector::importAndReport(const QString& text)
{
    const int before{signedCount()};
    QString error;
    const bool ok{importEnvelope(text, error)};
    showStatus(error, /*error=*/!ok || !error.isEmpty());
    if (!ok) return;
    UniValue json;
    if (!json.read(text.trimmed().toStdString())) return;
    const QString received_code{shared_mn::EnvelopeFingerprint(json)};
    const int added{signedCount() - before};
    showCodeLine(added > 0 ? tr("Received %1 · Code %2")
                                 .arg(SharedMnPlural(added, QT_TRANSLATE_NOOP("MnShareSession", "1 approval"),
                                                     QT_TRANSLATE_NOOP("MnShareSession", "%1 approvals")),
                                      received_code)
                           : tr("Received · Code %1").arg(received_code));
}

void SharedSigCollector::loadEnvelope()
{
    const QString filename{GUIUtil::getOpenFileName(this, tr("Open Request"), QString{},
                                                    tr("Shared masternode message (*.json);;All files (*)"), nullptr)};
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MAX_ENVELOPE_FILE_BYTES) {
        showStatus(tr("Could not read %1.").arg(filename), /*error=*/true);
        return;
    }
    const QString text{QString::fromUtf8(file.readAll())};
    file.close();
    importAndReport(text);
}

void SharedSigCollector::refreshUi()
{
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const bool approved{m_sigs.count(static_cast<int>(i)) > 0};
        m_board->setCell(static_cast<int>(i), 0,
                         approved ? SharedMnStatusBoard::State::Done : SharedMnStatusBoard::State::Pending,
                         approved ? tr("Approved by %1").arg(OwnerAddress(m_shares[i]))
                                  : tr("Waiting for %1").arg(OwnerAddress(m_shares[i])));
    }
    m_board->setSummary(tr("%1 of %2 approved").arg(m_sigs.size()).arg(m_shares.size()));

    m_copy_button->setEnabled(hasTransaction());
    m_save_button->setEnabled(hasTransaction());
}

void SharedSigCollector::showCodeLine(const QString& text)
{
    m_board->setLastReceived(text);
}

void SharedSigCollector::showStatus(const QString& message, bool error)
{
    ShowStatusLabel(m_status_label, message, error);
}

// ----------------------------------------------------------------------------
// SharedMnDialog
// ----------------------------------------------------------------------------

SharedMnDialog::SharedMnDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent) :
    QDialog(parent),
    m_node{node},
    m_wallet_model{wallet_model},
    m_protx_hash{entry.proTxHash()},
    m_shares{entry.shares()},
    m_early_penalty{entry.earlyPenalty()},
    m_early_period_blocks{entry.earlyPeriodBlocks()},
    m_registered_height{entry.registeredHeight()},
    m_v24_active{node.isV24Active()},
    m_sender{new ProTxSender(node, this)}
{
}

void SharedMnDialog::reject()
{
    // Esc or the window-manager close button must not dismiss the dialog
    // while runCommand() waits on an in-flight command; QDialog::closeEvent
    // keeps the window open when reject() leaves it visible
    if (m_busy) return;
    QDialog::reject();
}

bool SharedMnDialog::canSign() const
{
    return m_wallet_model != nullptr && !m_wallet_model->wallet().privateKeysDisabled();
}

bool SharedMnDialog::gateButton(QPushButton* button) const
{
    if (!m_wallet_model) {
        button->setEnabled(false);
        button->setToolTip(tr("No wallet is available."));
        return false;
    }
    if (m_wallet_model->wallet().privateKeysDisabled()) {
        button->setEnabled(false);
        button->setToolTip(tr("This wallet is watch-only and cannot sign."));
        return false;
    }
    if (!m_v24_active) {
        button->setEnabled(false);
        button->setToolTip(V24InactiveMessage());
        return false;
    }
    return true;
}

bool SharedMnDialog::runCommand(const QString& method, const UniValue& params, ProTxResult& result, QString& error)
{
    if (m_busy) {
        error = tr("Another command is still running.");
        return false;
    }
    if (!canSign()) {
        error = tr("This wallet is watch-only and cannot sign.");
        return false;
    }

    WalletModel::UnlockContext ctx(m_wallet_model->requestUnlock());
    if (!ctx.isValid()) {
        error = tr("Wallet unlock was cancelled.");
        return false;
    }

    m_busy = true;
    setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);

    ProTxResult outcome;
    QEventLoop loop;
    connect(m_sender, &ProTxSender::finished, &loop, [&](const ProTxResult& r) {
        outcome = r;
        loop.quit();
    });
    const bool started{m_sender->execute(method, params, m_wallet_model)};
    if (started) {
        // The unlock context cannot be moved, so wait here on the GUI thread to
        // keep it alive until the worker thread reports back
        loop.exec();
    }

    QApplication::restoreOverrideCursor();
    setEnabled(true);
    m_busy = false;

    if (!started) {
        error = tr("Another command is still running.");
        return false;
    }
    if (!outcome.ok) {
        error = outcome.message;
        return false;
    }
    result = outcome;
    return true;
}

bool SharedMnDialog::txInputsUnspent(const QString& tx_hex, QString& error)
{
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, tx_hex.toStdString())) {
        error = tr("The transaction is not valid transaction hex.");
        return false;
    }
    for (const CTxIn& txin : tx.vin) {
        Coin coin;
        if (m_node.getUnspentOutput(txin.prevout, coin)) continue;
        // Not in the confirmed UTXO set. A fee input funded from a
        // still-unconfirmed output of this wallet (e.g. fresh change selected
        // by the prepare call) is unspent and merely waiting for a
        // confirmation — only a positively spent input kills the session.
        if (m_wallet_model) {
            const auto coins{m_wallet_model->wallet().getCoins({txin.prevout})};
            if (coins.size() == 1 && !coins[0].is_spent && coins[0].depth_in_main_chain >= 0) continue;
        }
        error = tr("A coin this request needs (%1:%2) was spent. It can no longer be sent — start over.")
                    .arg(QString::fromStdString(txin.prevout.hash.ToString()))
                    .arg(txin.prevout.n);
        return false;
    }
    return true;
}

BitcoinAmountField* SharedMnDialog::makeFeeField(QWidget* parent) const
{
    return MakeFeeField(parent, displayUnit());
}

QString SharedMnDialog::shareLabel(int share_index) const
{
    return ShareOfLabel(share_index, m_shares.size());
}

BitcoinUnits::Unit SharedMnDialog::displayUnit() const
{
    return SharedMnDisplayUnit(m_wallet_model);
}

QComboBox* SharedMnDialog::makeMyShareCombo(QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    FillMyShareCombo(combo, m_shares, m_wallet_model);
    if (combo->count() == 0) {
        combo->setToolTip(NoShareKeysMessage());
    }
    return combo;
}

bool SharedMnDialog::adoptPreparedTx(SharedSigCollector* collector, const ProTxResult& result, QLabel* status)
{
    QString error;
    const QString tx_hex{QString::fromStdString(result.value.find_value("tx").get_str())};
    if (!collector->setTransaction(tx_hex, error)) {
        ShowStatusLabel(status, error, /*error=*/true);
        return false;
    }
    // Cross-check our native sign-hash mirror against the node's answer, so a
    // drifted mirror can never let a bad signature through verification
    const QString rpc_hash{QString::fromStdString(result.value.find_value("signHash").get_str())};
    if (collector->signHashHex().compare(rpc_hash, Qt::CaseInsensitive) != 0) {
        ShowStatusLabel(status,
                        tr("Internal error: the digest computed here does not match the node's. Do not approve; "
                           "please report this."),
                        /*error=*/true);
        return false;
    }
    return true;
}

bool SharedMnDialog::signWithWallet(SharedSigCollector* collector, QLabel* status)
{
    if (!collector->hasTransaction()) return false;

    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", collector->txHex().toStdString());

    ProTxResult result;
    QString error;
    if (!runCommand(shared_mn_rpc::SIGN, params, result, error)) {
        ShowStatusLabel(status, error, /*error=*/true);
        return false;
    }

    // shared_sign answers {type, terms, warning?, signatures: [{shareIndex, signature}]}
    const UniValue& signatures{result.value.isObject() ? result.value.find_value("signatures") : NullUniValue};
    if (const UniValue& warning{result.value.find_value("warning")}; warning.isStr() && !warning.get_str().empty()) {
        QMessageBox::warning(this, windowTitle(), QString::fromStdString(warning.get_str()));
    }
    QString sig_error;
    const int absorbed{collector->addSignatures(signatures, sig_error)};
    if (!sig_error.isEmpty()) {
        ShowStatusLabel(status, sig_error, /*error=*/true);
        return absorbed > 0;
    }
    ShowStatusLabel(status,
                    tr("Approved %1 with this wallet.")
                        .arg(SharedMnPlural(absorbed, QT_TRANSLATE_NOOP("MnShareSession", "1 share"),
                                            QT_TRANSLATE_NOOP("MnShareSession", "%1 shares"))),
                    /*error=*/false);
    return absorbed > 0;
}

bool SharedMnDialog::combineAndSubmit(SharedSigCollector* collector, QLabel* status, QLineEdit* result_edit,
                                      const QString& success_text, const QString& failure_hint)
{
    if (!collector->complete()) return false;

    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", collector->txHex().toStdString());
    params.pushKV("signatures", collector->signaturesArray());
    params.pushKV("submit", true);

    ProTxResult result;
    QString error;
    if (!runCommand(shared_mn_rpc::COMBINE, params, result, error)) {
        ShowStatusLabel(status, failure_hint.isEmpty() ? error : error + QLatin1Char('\n') + failure_hint,
                        /*error=*/true);
        return false;
    }

    result_edit->setText(QString::fromStdString(result.value.get_str()));
    result_edit->setVisible(true);
    ShowStatusLabel(status, success_text, /*error=*/false);
    return true;
}

bool SharedMnDialog::sessionAlive(SharedSigCollector* collector, QLabel* status)
{
    if (!collector->hasTransaction()) return true;
    if (QString liveness_error; !txInputsUnspent(collector->txHex(), liveness_error)) {
        ShowStatusLabel(status, liveness_error, /*error=*/true);
        return false;
    }
    return true;
}

bool SharedMnDialog::holdsAnyShareKey() const
{
    if (m_wallet_model == nullptr || m_wallet_model->wallet().privateKeysDisabled()) return false;
    return std::any_of(m_shares.begin(), m_shares.end(), [this](const interfaces::MnShare& share) {
        return m_wallet_model->wallet().isSpendable(PKHash(share.keyIDOwner));
    });
}

// ----------------------------------------------------------------------------
// DissolveDialog
// ----------------------------------------------------------------------------

DissolveDialog::DissolveDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry,
                               int current_height, QWidget* parent) :
    SharedMnDialog(node, wallet_model, entry, parent),
    m_current_height{current_height}
{
    setWindowTitle(tr("Dissolve Shared Masternode"));

    auto* layout = new QVBoxLayout(this);

    layout->addWidget(MakeProTxHeader(m_protx_hash, this));

    // A ProDisTx spending the collateral in the block that created it would be
    // rejected; every flow below builds against the confirmed masternode list
    if (m_current_height <= m_registered_height) {
        auto* wait_label = new QLabel(tr("This masternode was registered in block %1 and the chain is still at "
                                         "block %2. Wait for one confirmation, then reopen this window.")
                                          .arg(m_registered_height)
                                          .arg(m_current_height),
                                      this);
        wait_label->setWordWrap(true);
        layout->addWidget(wait_label);
    } else {
        m_tabs = new QTabWidget(this);
        m_tabs->addTab(buildNowTab(), tr("Dissolve Now"));
        m_together_tab_index = m_tabs->addTab(buildTogetherTab(), tr("Dissolve Together"));
        m_standby_tab_index = m_tabs->addTab(buildStandbyTab(), tr("Standby Dissolution"));
        layout->addWidget(m_tabs);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    GUIUtil::updateFonts();
    // Payout tables, refund addresses and the approvals board are all wider and
    // taller than a fixed dialog: open at the size the tabs actually need, so
    // the payout table, the accept box and the submit button are all visible
    SharedMnFitWrappedLabels(this);
    SharedMnSizeFromContent(this, 760);
    if (m_now_actor) updateNowPreview();
    refreshTogetherUi();
}

void DissolveDialog::selectStandbyTab()
{
    if (m_tabs != nullptr && m_standby_tab_index >= 0) {
        m_tabs->setCurrentIndex(m_standby_tab_index);
    }
}

QWidget* DissolveDialog::buildNowTab()
{
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);
    layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);

    layout->addWidget(MasternodeWidgetUtil::makeHint(
        tr("Ends the masternode on your own. Everyone gets their principal back at their refund address."), tab));

    auto* form = new QFormLayout();
    m_now_actor = makeMyShareCombo(tab);
    m_now_actor->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_now_actor, qOverload<int>(&QComboBox::currentIndexChanged), this, &DissolveDialog::updateNowPreview);
    form->addRow(tr("Your share:"), m_now_actor);

    m_now_fee = makeFeeField(tab);
    m_now_fee->setToolTip(tr("Transaction fee, paid from your share's refund."));
    connect(m_now_fee, &BitcoinAmountField::valueChanged, this, &DissolveDialog::updateNowPreview);
    form->addRow(tr("Fee:"), m_now_fee);
    layout->addLayout(form);

    m_now_early_label = new QLabel(tab);
    m_now_early_label->setWordWrap(true);
    layout->addWidget(m_now_early_label);

    m_now_nudge_label = new QLabel(tab);
    m_now_nudge_label->setWordWrap(true);
    m_now_nudge_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    m_now_nudge_label->setVisible(false);
    layout->addWidget(m_now_nudge_label);

    m_now_table = MakePayoutTable(tab);
    layout->addWidget(m_now_table);

    m_now_error_label = new QLabel(tab);
    m_now_error_label->setWordWrap(true);
    m_now_error_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
    m_now_error_label->setVisible(false);
    layout->addWidget(m_now_error_label);

    m_now_accept = new QCheckBox(tab);
    m_now_accept->setVisible(false);
    connect(m_now_accept, &QCheckBox::toggled, this, &DissolveDialog::updateNowPreview);
    layout->addWidget(m_now_accept);

    m_now_submit = new QPushButton(tr("Dissolve Now"), tab);
    connect(m_now_submit, &QPushButton::clicked, this, &DissolveDialog::submitNow);
    layout->addWidget(m_now_submit, 0, Qt::AlignLeft);

    m_now_reason = MasternodeWidgetUtil::makeHint(QString{}, tab);
    m_now_reason->setVisible(false);
    layout->addWidget(m_now_reason);

    m_now_result = new QLineEdit(tab);
    m_now_result->setReadOnly(true);
    m_now_result->setPlaceholderText(tr("Transaction ID"));
    m_now_result->setVisible(false);
    layout->addWidget(m_now_result);

    m_now_status = new QLabel(tab);
    m_now_status->setWordWrap(true);
    m_now_status->setVisible(false);
    layout->addWidget(m_now_status);

    layout->addStretch();
    return MakeScroll(tab, this);
}

void DissolveDialog::updateNowPreview()
{
    const bool usable{gateButton(m_now_submit) && m_now_actor->count() > 0};
    if (m_now_actor->count() == 0) {
        m_now_submit->setEnabled(false);
        ShowReason(m_now_submit, m_now_reason, NoShareKeysMessage());
    }

    const int actor{m_now_actor->currentIndex() >= 0 ? m_now_actor->currentData().toInt() : -1};
    if (actor < 0) {
        m_now_table->setRowCount(0);
        return;
    }

    std::vector<CAmount> amounts;
    amounts.reserve(m_shares.size());
    for (const auto& share : m_shares) {
        amounts.push_back(share.amount);
    }
    const auto preview{MnShareSession::PenaltyPreviewFor(amounts, actor, m_early_penalty, m_early_period_blocks,
                                                         m_now_fee->value(), m_current_height, m_registered_height)};

    m_now_error_label->setVisible(!preview.valid);
    if (!preview.valid) {
        m_now_error_label->setText(preview.error);
        m_now_table->setRowCount(0);
        m_now_accept->setVisible(false);
        m_now_nudge_label->setVisible(false);
        m_now_early_label->clear();
        m_now_submit->setEnabled(false);
        return;
    }

    // Penalty-free once the transaction would confirm at the boundary height
    const int blocks_left{preview.penaltyFreeHeight - m_current_height - 1};
    if (preview.early) {
        m_now_early_label->setText(tr("Early exit: you pay the %1 penalty. Free after block %2 (%3, %4).")
                                       .arg(SharedMnFormatAmount(displayUnit(), preview.penalty))
                                       .arg(preview.penaltyFreeHeight)
                                       .arg(ApproxBlockDate(std::max(blocks_left, 0)),
                                            MnShareSession::HumanEarlyPeriod(
                                                static_cast<uint32_t>(std::max(blocks_left, 0)))));
        m_now_accept->setText(tr("I accept paying the %1 early-exit penalty").arg(SharedMnFormatAmount(displayUnit(), preview.penalty)));
        m_now_accept->setVisible(true);
        m_now_nudge_label->setVisible(blocks_left <= NUDGE_BOUNDARY_BLOCKS);
        m_now_nudge_label->setText(tr("Only %1 until dissolving is free. Consider waiting, or use Dissolve Together — "
                                      "with everyone's approval it is free now.")
                                       .arg(MnShareSession::HumanEarlyPeriod(
                                           static_cast<uint32_t>(std::max(blocks_left, 0)))));
    } else {
        m_now_early_label->setText(tr("The early period has ended — dissolving now is free."));
        m_now_accept->setVisible(false);
        m_now_accept->setChecked(false);
        m_now_nudge_label->setVisible(false);
    }

    m_now_table->setRowCount(static_cast<int>(m_shares.size()));
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const bool is_actor{static_cast<int>(i) == actor};
        const QString label{shareLabel(static_cast<int>(i))};
        FillPayoutRow(m_now_table, static_cast<int>(i), is_actor ? tr("%1 (you)").arg(label) : label,
                      ScriptAddress(m_shares[i].scriptRefund),
                      SharedMnFormatAmount(displayUnit(), preview.payouts[i]), is_actor);
    }

    if (usable) {
        m_now_submit->setEnabled(!preview.early || m_now_accept->isChecked());
        ShowReason(m_now_submit, m_now_reason,
                   preview.early && !m_now_accept->isChecked() ? tr("Tick the box to accept the penalty first.")
                                                               : QString{});
    }
}

void DissolveDialog::submitNow()
{
    if (m_now_actor->currentIndex() < 0) return;
    const int actor{m_now_actor->currentData().toInt()};

    // Re-derive the early decision the RPC will make so payPenalty is
    // explicit — from a fresh tip height, not the height captured at dialog
    // open: crossing the penalty-free boundary while the dialog was open must
    // never submit an avoidable payPenalty=true.
    const auto is_early = [this](int height) -> bool {
        return static_cast<int64_t>(height) + 1 - m_registered_height < static_cast<int64_t>(m_early_period_blocks);
    };
    const auto refresh_on_boundary_crossing = [this, &is_early](int fresh_height) {
        const bool crossed{is_early(fresh_height) != is_early(m_current_height)};
        if (fresh_height != m_current_height) {
            m_current_height = fresh_height;
            updateNowPreview();
        }
        if (crossed) {
            ShowStatusLabel(m_now_status,
                            tr("The chain crossed the penalty-free boundary while this dialog was open. The preview "
                               "above has been refreshed — review it and press Dissolve Now again."),
                            /*error=*/false);
        }
        return crossed;
    };
    if (refresh_on_boundary_crossing(m_node.getNumBlocks())) return;
    const bool early{is_early(m_current_height)};
    if (early && !m_now_accept->isChecked()) return;

    if (QMessageBox::question(this, windowTitle(),
                              tr("Dissolve this shared masternode now? Every participant's principal returns to "
                                 "their refund address and the masternode stops earning. This cannot be undone.")) !=
        QMessageBox::Yes) {
        return;
    }

    // The confirmation prompt can also stay open across the boundary
    if (refresh_on_boundary_crossing(m_node.getNumBlocks())) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    params.pushKV("actorIndex", actor);
    params.pushKV("fee", static_cast<int64_t>(m_now_fee->value()));
    params.pushKV("submit", true);
    params.pushKV("payPenalty", early);

    ProTxResult result;
    QString error;
    if (!runCommand(shared_mn_rpc::DISSOLVE, params, result, error)) {
        ShowStatusLabel(m_now_status, error, /*error=*/true);
        return;
    }

    m_now_result->setText(QString::fromStdString(result.value.get_str()));
    m_now_result->setVisible(true);
    ShowStatusLabel(m_now_status, tr("The dissolution was sent."), /*error=*/false);
    m_now_submit->setEnabled(false);
    m_now_actor->setEnabled(false);
    m_now_fee->setEnabled(false);
    m_now_accept->setEnabled(false);
}

QWidget* DissolveDialog::buildTogetherTab()
{
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);
    layout->setSpacing(MasternodeWidgetUtil::GROUP_SPACING);

    auto* intro = MasternodeWidgetUtil::makeHint(
        tr("Free at any time, but every share owner must approve. One person prepares the request; everyone else "
           "pastes it, approves, and copies back a reply."),
        tab);
    layout->addWidget(intro);

    // 1. Request
    auto* request_card = MasternodeWidgetUtil::makeCard(tab);
    auto* request_layout = new QVBoxLayout(request_card);
    request_layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);
    request_layout->addWidget(MasternodeWidgetUtil::makeTitle(tr("1. Request"), request_card));

    auto* form = new QFormLayout();
    m_un_actor = makeMyShareCombo(request_card);
    m_un_actor->setToolTip(tr("The share the transaction fee is deducted from."));
    form->addRow(tr("Fee paid by:"), m_un_actor);
    m_un_fee = makeFeeField(request_card);
    form->addRow(tr("Fee:"), m_un_fee);
    request_layout->addLayout(form);

    m_un_prepare = new QPushButton(tr("Prepare Request"), request_card);
    connect(m_un_prepare, &QPushButton::clicked, this, &DissolveDialog::prepareTogether);
    request_layout->addWidget(m_un_prepare, 0, Qt::AlignLeft);
    layout->addWidget(request_card);

    // 2. Approvals
    auto* approvals_card = MasternodeWidgetUtil::makeCard(tab);
    auto* approvals_layout = new QVBoxLayout(approvals_card);
    approvals_layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);
    approvals_layout->addWidget(MasternodeWidgetUtil::makeTitle(tr("2. Approvals"), approvals_card));
    approvals_layout->addWidget(MasternodeWidgetUtil::makeHint(
        tr("Send the same request to every share owner. Replies can come back in any order."), approvals_card));

    // Approving is signing away the collateral, so the request's own outputs
    // are on screen next to the button that approves them
    m_un_payouts_title = MasternodeWidgetUtil::makeTitle(tr("This request pays out"), approvals_card);
    m_un_payouts_title->setVisible(false);
    approvals_layout->addWidget(m_un_payouts_title);
    m_un_table = MakePayoutTable(approvals_card);
    m_un_table->setVisible(false);
    approvals_layout->addWidget(m_un_table);
    m_un_fee_label = MasternodeWidgetUtil::makeHint(QString{}, approvals_card);
    m_un_fee_label->setVisible(false);
    approvals_layout->addWidget(m_un_fee_label);

    m_un_collector = new SharedSigCollector(SharedSigCollector::Kind::Dissolve, m_protx_hash, m_shares, m_wallet_model,
                                            approvals_card);
    connect(m_un_collector, &SharedSigCollector::changed, this, &DissolveDialog::refreshTogetherUi);
    approvals_layout->addWidget(m_un_collector);

    m_un_approve = new QPushButton(tr("Approve and Copy Reply"), approvals_card);
    connect(m_un_approve, &QPushButton::clicked, this, &DissolveDialog::approveTogether);
    approvals_layout->addWidget(m_un_approve, 0, Qt::AlignLeft);
    layout->addWidget(approvals_card);

    // 3. Send
    auto* send_card = MasternodeWidgetUtil::makeCard(tab);
    auto* send_layout = new QVBoxLayout(send_card);
    send_layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);
    send_layout->addWidget(MasternodeWidgetUtil::makeTitle(tr("3. Send"), send_card));

    m_un_send = new QPushButton(tr("Send Dissolution"), send_card);
    connect(m_un_send, &QPushButton::clicked, this, &DissolveDialog::sendTogether);
    send_layout->addWidget(m_un_send, 0, Qt::AlignLeft);

    m_un_reason = MasternodeWidgetUtil::makeHint(QString{}, send_card);
    m_un_reason->setVisible(false);
    send_layout->addWidget(m_un_reason);

    m_un_result = new QLineEdit(send_card);
    m_un_result->setReadOnly(true);
    m_un_result->setPlaceholderText(tr("Transaction ID"));
    m_un_result->setVisible(false);
    send_layout->addWidget(m_un_result);
    layout->addWidget(send_card);

    m_un_status = new QLabel(tab);
    m_un_status->setWordWrap(true);
    m_un_status->setVisible(false);
    layout->addWidget(m_un_status);

    layout->addStretch();
    return MakeScroll(tab, this);
}

void DissolveDialog::updateTogetherPayouts()
{
    if (m_un_table == nullptr) return;

    const auto hide = [this] {
        m_un_table->setRowCount(0);
        m_un_table->setVisible(false);
        m_un_payouts_title->setVisible(false);
        m_un_fee_label->setVisible(false);
    };

    CMutableTransaction tx;
    if (!m_un_collector->hasTransaction() || !DecodeHexTx(tx, m_un_collector->txHex().toStdString())) return hide();
    const auto ptx{GetTxPayload<CProDisTx>(tx)};
    if (!ptx) return hide();
    // setTransaction() only adopts the principal-returning template, so a
    // request that is on screen at all has payouts worth showing
    const SharedMnDissolution dissolution{SharedMnReadDissolution(tx, ptx->actorIndex, m_shares)};
    if (!dissolution.returnsPrincipal) return hide();

    const int actor{static_cast<int>(ptx->actorIndex)};
    m_un_table->setRowCount(static_cast<int>(m_shares.size()));
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const bool mine{m_wallet_model != nullptr && !m_wallet_model->wallet().privateKeysDisabled() &&
                        m_wallet_model->wallet().isSpendable(PKHash(m_shares[i].keyIDOwner))};
        const QString label{shareLabel(static_cast<int>(i))};
        FillPayoutRow(m_un_table, static_cast<int>(i), mine ? tr("%1 (you)").arg(label) : label,
                      ScriptAddress(m_shares[i].scriptRefund),
                      SharedMnFormatAmount(displayUnit(), dissolution.payouts[i]), mine);
    }
    m_un_fee_label->setText(tr("Fee: %1, paid out of %2.")
                                .arg(SharedMnFormatAmount(displayUnit(), dissolution.fee), shareLabel(actor)));
    m_un_table->setVisible(true);
    m_un_payouts_title->setVisible(true);
    m_un_fee_label->setVisible(true);
}

void DissolveDialog::refreshTogetherUi()
{
    if (!m_un_prepare) return;
    updateTogetherPayouts();
    const bool alive{sessionAlive(m_un_collector, m_un_status)};
    const bool has_tx{m_un_collector->hasTransaction()};

    if (gateButton(m_un_prepare)) {
        m_un_prepare->setEnabled(alive && !has_tx && m_un_actor->count() > 0);
        if (m_un_actor->count() == 0) m_un_prepare->setToolTip(NoShareKeysMessage());
    }
    if (gateButton(m_un_approve)) {
        m_un_approve->setEnabled(alive && has_tx && holdsAnyShareKey() && !m_un_collector->complete());
        m_un_approve->setToolTip(!holdsAnyShareKey() ? NoShareKeysMessage()
                                : !has_tx           ? tr("Prepare or paste a request first.")
                                                    : QString{});
    }
    if (gateButton(m_un_send)) {
        m_un_send->setEnabled(alive && m_un_collector->complete());
    }
    ShowReason(m_un_send, m_un_reason,
               !has_tx ? tr("Prepare or paste a request first.")
                       : tr("Every share must approve first (%1 of %2 so far).")
                             .arg(m_un_collector->signedCount())
                             .arg(m_shares.size()));
}

void DissolveDialog::prepareTogether()
{
    if (m_un_actor->currentIndex() < 0) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    params.pushKV("actorIndex", m_un_actor->currentData().toInt());
    params.pushKV("fee", static_cast<int64_t>(m_un_fee->value()));

    ProTxResult result;
    QString error;
    if (!runCommand(shared_mn_rpc::DISSOLVE_PREPARE, params, result, error)) {
        ShowStatusLabel(m_un_status, error, /*error=*/true);
        return;
    }

    if (!adoptPreparedTx(m_un_collector, result, m_un_status)) return;
    m_un_fee->setEnabled(false);
    m_un_actor->setEnabled(false);
    m_un_prepare->setEnabled(false);

    // Approving with our own share owner keys needs no decision from the user
    signWithWallet(m_un_collector, m_un_status);
    refreshTogetherUi();
}

void DissolveDialog::approveTogether()
{
    if (!signWithWallet(m_un_collector, m_un_status)) return;
    m_un_collector->copyEnvelope();
    refreshTogetherUi();
}

void DissolveDialog::sendTogether()
{
    if (!combineAndSubmit(m_un_collector, m_un_status, m_un_result, tr("The dissolution was sent."),
                          /*failure_hint=*/QString{})) {
        return;
    }
    m_un_approve->setEnabled(false);
    m_un_send->setEnabled(false);
}

void DissolveDialog::preloadEnvelope(const QString& text)
{
    if (m_tabs == nullptr || m_un_collector == nullptr) return;
    if (m_together_tab_index >= 0) m_tabs->setCurrentIndex(m_together_tab_index);
    m_un_collector->importAndReport(text);
    refreshTogetherUi();
}

QWidget* DissolveDialog::buildStandbyTab()
{
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);
    layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);

    layout->addWidget(MasternodeWidgetUtil::makeHint(
        tr("A signed dissolution you keep offline and never broadcast unless you need it. It never expires and works "
           "even if this wallet is lost."),
        tab));

    auto* form = new QFormLayout();
    m_sb_actor = makeMyShareCombo(tab);
    form->addRow(tr("Your share:"), m_sb_actor);
    m_sb_fee = makeFeeField(tab);
    form->addRow(tr("Fee:"), m_sb_fee);
    layout->addLayout(form);

    m_sb_create = new QPushButton(tr("Create Standby Dissolution…"), tab);
    connect(m_sb_create, &QPushButton::clicked, this, &DissolveDialog::createStandby);
    if (gateButton(m_sb_create) && m_sb_actor->count() == 0) {
        m_sb_create->setEnabled(false);
        m_sb_create->setToolTip(NoShareKeysMessage());
    }
    layout->addWidget(m_sb_create, 0, Qt::AlignLeft);

    // One read-only view plus a Copy button per variant, filled once both have
    // been generated
    const auto build_view = [tab, layout](const QString& title, const QString& placeholder) {
        layout->addWidget(MasternodeWidgetUtil::makeTitle(title, tab));
        auto* view = new QPlainTextEdit(tab);
        view->setReadOnly(true);
        view->setFont(GUIUtil::fixedPitchFont());
        view->setMaximumHeight(70);
        view->setPlaceholderText(placeholder);
        layout->addWidget(view);
        auto* copy = new QPushButton(tr("Copy"), tab);
        SharedMnMakeSecondary(copy);
        connect(copy, &QPushButton::clicked, tab, [view] {
            if (!view->toPlainText().isEmpty()) GUIUtil::setClipboard(view->toPlainText());
        });
        layout->addWidget(copy, 0, Qt::AlignLeft);
        return view;
    };

    const int penalty_free_height{m_registered_height + static_cast<int>(m_early_period_blocks)};
    const int blocks_left{std::max(penalty_free_height - m_current_height - 1, 0)};
    m_sb_full_view = build_view(blocks_left > 0
                                    ? tr("Full principal — valid from block %1 (~%2)")
                                          .arg(penalty_free_height)
                                          .arg(ApproxBlockDate(blocks_left))
                                    : tr("Full principal — valid now, the early period has ended"),
                                tr("Not created yet"));
    m_sb_immediate_view = build_view(tr("Immediate — pays the %1 penalty")
                                         .arg(SharedMnFormatAmount(displayUnit(), m_early_penalty)),
                                     tr("Not created yet"));

    layout->addWidget(MasternodeWidgetUtil::makeHint(
        tr("Store it with your refund-address backup, separately from this wallet."), tab));

    m_sb_stored_label = new QLabel(tab);
    m_sb_stored_label->setWordWrap(true);
    m_sb_stored_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_SUCCESS));
    const auto saved{MasternodeStandby::SavedDate(m_protx_hash)};
    m_sb_stored_label->setVisible(saved.saved);
    m_sb_stored_label->setText(tr("Saved on %1").arg(saved.date));
    layout->addWidget(m_sb_stored_label);

    m_sb_status = new QLabel(tab);
    m_sb_status->setWordWrap(true);
    m_sb_status->setVisible(false);
    layout->addWidget(m_sb_status);

    layout->addStretch();
    return MakeScroll(tab, this);
}

void DissolveDialog::createStandby()
{
    if (m_sb_actor->currentIndex() < 0) {
        ShowStatusLabel(m_sb_status, NoShareKeysMessage(), /*error=*/true);
        return;
    }
    const int share_index{m_sb_actor->currentData().toInt()};

    // Both variants describe the same share and fee, so they are generated
    // together: the user should never have to decide which one to keep
    const auto generate = [this, share_index](bool pay_penalty, QString& hex) {
        UniValue params(UniValue::VOBJ);
        params.pushKV("proTxHash", m_protx_hash.toStdString());
        params.pushKV("actorIndex", share_index);
        params.pushKV("fee", static_cast<int64_t>(m_sb_fee->value()));
        params.pushKV("submit", false);
        params.pushKV("payPenalty", pay_penalty);

        ProTxResult result;
        QString error;
        if (!runCommand(shared_mn_rpc::DISSOLVE, params, result, error)) {
            ShowStatusLabel(m_sb_status, error, /*error=*/true);
            return false;
        }
        hex = QString::fromStdString(result.value.get_str());
        return true;
    };

    QString full;
    QString immediate;
    if (!generate(/*pay_penalty=*/false, full)) return;
    if (!generate(/*pay_penalty=*/true, immediate)) return;

    m_sb_share_index = share_index;
    m_sb_hex_full = full;
    m_sb_hex_immediate = immediate;
    m_sb_full_view->setPlainText(m_sb_hex_full);
    m_sb_immediate_view->setPlainText(m_sb_hex_immediate);

    const QString filename{GUIUtil::getSaveFileName(this, tr("Save Standby Dissolution"), standbyFileName(),
                                                    tr("Text file (*.txt)"), nullptr)};
    if (filename.isEmpty()) {
        ShowStatusLabel(m_sb_status,
                        tr("Both transactions are ready below, but nothing was saved. Copy them or create the file "
                           "again."),
                        /*error=*/true);
        return;
    }
    if (QString error; !writeStandbyFile(filename, error)) {
        ShowStatusLabel(m_sb_status, error, /*error=*/true);
        return;
    }
    recordStandbySaved();
    ShowStatusLabel(m_sb_status, tr("Saved to %1.").arg(filename), /*error=*/false);
}

QString DissolveDialog::standbyFileName() const
{
    return QStringLiteral("standby-dissolution-%1-share%2.txt")
        .arg(m_protx_hash.left(8))
        .arg(m_sb_share_index + 1);
}

QString DissolveDialog::standbyFileText() const
{
    const int penalty_free_height{m_registered_height + static_cast<int>(m_early_period_blocks)};
    const int blocks_left{std::max(penalty_free_height - m_current_height - 1, 0)};

    QStringList lines;
    lines << tr("Dash shared masternode — standby dissolution");
    lines << tr("Masternode: %1").arg(m_protx_hash);
    lines << shareLabel(m_sb_share_index);
    lines << tr("Created: %1").arg(QDate::currentDate().toString(Qt::ISODate));
    lines << QString{};
    lines << tr("Each block below is a complete signed transaction. Broadcast one of them with "
                "\"sendrawtransaction\" only when you want to end the masternode and get your principal back. "
                "They never expire. Store this file with your refund-address backup, separately from the wallet.");
    lines << QString{};
    lines << (blocks_left > 0 ? tr("FULL PRINCIPAL — valid from block %1 (~%2)")
                                    .arg(penalty_free_height)
                                    .arg(ApproxBlockDate(blocks_left))
                              : tr("FULL PRINCIPAL — valid now, the early period has ended"));
    lines << m_sb_hex_full;
    lines << QString{};
    lines << tr("IMMEDIATE — pays the %1 early-exit penalty, valid right away")
                 .arg(SharedMnFormatAmount(displayUnit(), m_early_penalty));
    lines << m_sb_hex_immediate;
    lines << QString{};
    return lines.join(QLatin1Char('\n'));
}

bool DissolveDialog::writeStandbyFile(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = tr("Could not write to %1.").arg(path);
        return false;
    }
    QTextStream out(&file);
    out << standbyFileText();
    file.close();
    return true;
}

void DissolveDialog::recordStandbySaved()
{
    // A file was written, so any earlier "nothing was saved" line is no longer
    // true and must not sit beside the green one
    ShowStatusLabel(m_sb_status, QString{}, /*error=*/false);
    QSettings settings;
    settings.setValue(QStringLiteral("sharedmn/standby/") + m_protx_hash,
                      QDate::currentDate().toString(Qt::ISODate));
    m_sb_stored_label->setText(tr("Saved on %1").arg(QDate::currentDate().toString(Qt::ISODate)));
    m_sb_stored_label->setVisible(true);
}

// ----------------------------------------------------------------------------
// RotateSharedKeysDialog
// ----------------------------------------------------------------------------

RotateSharedKeysDialog::RotateSharedKeysDialog(interfaces::Node& node, WalletModel* wallet_model,
                                               const MasternodeEntry& entry, QWidget* parent) :
    SharedMnDialog(node, wallet_model, entry, parent),
    m_current_operator_pubkey{entry.operatorPubKey(/*legacy_scheme=*/false)},
    m_current_voting_address{entry.votingAddress()}
{
    setWindowTitle(tr("Rotate Operator or Voting Key"));

    auto* outer = new QVBoxLayout(this);
    auto* body = new QWidget(this);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, MasternodeWidgetUtil::ROW_SPACING, 0);
    layout->setSpacing(MasternodeWidgetUtil::GROUP_SPACING);

    auto* intro = MasternodeWidgetUtil::makeHint(
        tr("Changing these keys needs an approval from every share owner. One person starts the request; everyone "
           "else pastes it, approves, and copies back a reply."),
        body);
    layout->addWidget(intro);
    layout->addWidget(MakeProTxHeader(m_protx_hash, body));

    // 1. New keys
    auto* keys_card = MasternodeWidgetUtil::makeCard(body);
    auto* keys_layout = new QVBoxLayout(keys_card);
    keys_layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);
    keys_layout->addWidget(MasternodeWidgetUtil::makeTitle(tr("1. New keys"), keys_card));

    auto* form = new QFormLayout();
    auto* operator_row = new QWidget(keys_card);
    auto* operator_layout = new QHBoxLayout(operator_row);
    operator_layout->setContentsMargins(0, 0, 0, 0);
    m_operator_edit = new QLineEdit(operator_row);
    m_operator_edit->setFont(GUIUtil::fixedPitchFont());
    m_operator_edit->setPlaceholderText(tr("Leave blank to keep the current operator key"));
    connect(m_operator_edit, &QLineEdit::textChanged, this, &RotateSharedKeysDialog::validateForm);
    operator_layout->addWidget(m_operator_edit, /*stretch=*/1);
    // This key is the one thing the new operator needs out of this window, and
    // it stays readable after the request is prepared
    m_operator_copy = new QPushButton(tr("Copy"), operator_row);
    SharedMnMakeSecondary(m_operator_copy);
    connect(m_operator_copy, &QPushButton::clicked, this, [this] {
        if (const QString key{operatorKeyInput()}; !key.isEmpty()) GUIUtil::setClipboard(key);
    });
    operator_layout->addWidget(m_operator_copy);
    form->addRow(tr("New operator key:"), operator_row);

    m_pose_warning = new QLabel(tr("The masternode is banned the moment the operator key changes, until the new "
                                   "operator sends a service update."),
                                keys_card);
    m_pose_warning->setWordWrap(true);
    m_pose_warning->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    m_pose_warning->setVisible(false);
    form->addRow(m_pose_warning);

    m_voting_edit = new QValidatedLineEdit(keys_card);
    GUIUtil::setupAddressWidget(m_voting_edit, this);
    m_voting_edit->setPlaceholderText(tr("Leave blank to keep the current voting address"));
    connect(m_voting_edit, &QLineEdit::textChanged, this, &RotateSharedKeysDialog::validateForm);
    form->addRow(tr("New voting address:"), m_voting_edit);

    m_fee_source = new FeeSourcePicker(keys_card);
    m_fee_source->setWalletModel(wallet_model);
    m_fee_source->refresh();
    connect(m_fee_source, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &RotateSharedKeysDialog::validateForm);
    form->addRow(tr("Fee source:"), m_fee_source);
    keys_layout->addLayout(form);

    m_requested_change = MasternodeWidgetUtil::makeValue(QString{}, keys_card);
    m_requested_change->setVisible(false);
    keys_layout->addWidget(m_requested_change);

    m_prepare_button = new QPushButton(tr("Prepare Request"), keys_card);
    connect(m_prepare_button, &QPushButton::clicked, this, &RotateSharedKeysDialog::prepare);
    keys_layout->addWidget(m_prepare_button, 0, Qt::AlignLeft);

    m_finish_banner = new QLabel(tr("Finish on this wallet — it pays the fee and must send the final transaction."),
                                 keys_card);
    m_finish_banner->setWordWrap(true);
    m_finish_banner->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    m_finish_banner->setVisible(false);
    keys_layout->addWidget(m_finish_banner);
    layout->addWidget(keys_card);

    // 2. Approvals
    auto* approvals_card = MasternodeWidgetUtil::makeCard(body);
    auto* approvals_layout = new QVBoxLayout(approvals_card);
    approvals_layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);
    approvals_layout->addWidget(MasternodeWidgetUtil::makeTitle(tr("2. Approvals"), approvals_card));
    approvals_layout->addWidget(MasternodeWidgetUtil::makeHint(
        tr("Send the same request to every share owner. Replies can come back in any order."), approvals_card));

    m_collector = new SharedSigCollector(SharedSigCollector::Kind::Registrar, m_protx_hash, m_shares, m_wallet_model,
                                         approvals_card);
    connect(m_collector, &SharedSigCollector::changed, this, &RotateSharedKeysDialog::validateForm);
    approvals_layout->addWidget(m_collector);

    m_approve_button = new QPushButton(tr("Approve and Copy Reply"), approvals_card);
    connect(m_approve_button, &QPushButton::clicked, this, &RotateSharedKeysDialog::approveAndCopy);
    approvals_layout->addWidget(m_approve_button, 0, Qt::AlignLeft);
    layout->addWidget(approvals_card);

    // 3. Send
    auto* send_card = MasternodeWidgetUtil::makeCard(body);
    auto* send_layout = new QVBoxLayout(send_card);
    send_layout->setSpacing(MasternodeWidgetUtil::ROW_SPACING);
    send_layout->addWidget(MasternodeWidgetUtil::makeTitle(tr("3. Send"), send_card));

    m_send_button = new QPushButton(tr("Send Rotation"), send_card);
    connect(m_send_button, &QPushButton::clicked, this, &RotateSharedKeysDialog::sendRotation);
    send_layout->addWidget(m_send_button, 0, Qt::AlignLeft);

    m_reason_label = MasternodeWidgetUtil::makeHint(QString{}, send_card);
    m_reason_label->setVisible(false);
    send_layout->addWidget(m_reason_label);

    m_result_edit = new QLineEdit(send_card);
    m_result_edit->setReadOnly(true);
    m_result_edit->setPlaceholderText(tr("Transaction ID"));
    m_result_edit->setVisible(false);
    send_layout->addWidget(m_result_edit);
    layout->addWidget(send_card);

    m_status_label = new QLabel(body);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    layout->addWidget(m_status_label);
    layout->addStretch();

    outer->addWidget(MakeScroll(body, this));

    auto* close_buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(close_buttons);

    GUIUtil::updateFonts();
    // Operator keys, addresses and the approvals board are all wider than a
    // fixed dialog; nothing on this window may clip at its natural size.
    // Measure with the warning lines up, because preparing a request shows
    // them and the window must not have to grow (or clip "3. Send") then.
    SharedMnFitWrappedLabels(this);
    for (QWidget* const later : {static_cast<QWidget*>(m_pose_warning), static_cast<QWidget*>(m_finish_banner),
                                 static_cast<QWidget*>(m_reason_label), static_cast<QWidget*>(m_result_edit),
                                 static_cast<QWidget*>(m_status_label)}) {
        later->setVisible(true);
    }
    SharedMnSizeFromContent(this, 760);
    validateForm();
}

QString RotateSharedKeysDialog::operatorKeyInput() const
{
    return m_operator_edit->text().simplified().remove(QLatin1Char(' '));
}

bool RotateSharedKeysDialog::IsValidOperatorKey(const QString& hex)
{
    CBLSPublicKey key;
    return key.SetHexStr(hex.toStdString(), /*specificLegacyScheme=*/false) && key.IsValid();
}

void RotateSharedKeysDialog::preloadEnvelope(const QString& text)
{
    m_collector->importAndReport(text);
    showRequestedChange();
    validateForm();
}

bool RotateSharedKeysDialog::preparedByThisWallet() const
{
    if (m_wallet_model == nullptr || !m_collector->hasTransaction()) return false;
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, m_collector->txHex().toStdString()) || tx.vin.empty()) return false;
    std::vector<COutPoint> outpoints;
    outpoints.reserve(tx.vin.size());
    for (const CTxIn& input : tx.vin) {
        outpoints.push_back(input.prevout);
    }
    const auto coins{m_wallet_model->wallet().getCoins(outpoints)};
    if (coins.size() != outpoints.size()) return false;
    return std::all_of(coins.begin(), coins.end(), [this](const interfaces::WalletTxOut& coin) {
        return coin.depth_in_main_chain >= 0 && m_wallet_model->wallet().isSpendable(coin.txout.scriptPubKey);
    });
}

void RotateSharedKeysDialog::showRequestedChange()
{
    // A wallet that did not prepare cannot edit the request; it reads what is
    // being asked for out of the transaction it just imported
    const bool foreign_request{m_collector->hasTransaction() && !preparedByThisWallet()};
    m_requested_change->setVisible(foreign_request);
    m_operator_edit->setVisible(!foreign_request);
    m_voting_edit->setVisible(!foreign_request);
    m_fee_source->setVisible(!foreign_request);
    m_prepare_button->setVisible(!foreign_request);
    if (!foreign_request) return;

    CMutableTransaction tx;
    QStringList lines;
    if (DecodeHexTx(tx, m_collector->txHex().toStdString())) {
        if (const auto ptx{GetTxPayload<CProUpSharedRegTx>(tx)}) {
            const QString new_operator{
                QString::fromStdString(ptx->pubKeyOperator.Get().ToString(/*specificLegacyScheme=*/false))};
            const QString new_voting{QString::fromStdString(EncodeDestination(PKHash(ptx->keyIDVoting)))};
            lines << tr("Operator key: %1")
                         .arg(new_operator.compare(m_current_operator_pubkey, Qt::CaseInsensitive) == 0
                                  ? tr("unchanged")
                                  : tr("→ %1").arg(new_operator));
            lines << tr("Voting address: %1")
                         .arg(new_voting == m_current_voting_address ? tr("unchanged") : tr("→ %1").arg(new_voting));
        }
    }
    m_requested_change->setText(lines.join(QLatin1Char('\n')));
}

void RotateSharedKeysDialog::validateForm()
{
    m_pose_warning->setVisible(!m_collector->hasTransaction() && !operatorKeyInput().isEmpty());
    m_operator_copy->setEnabled(!operatorKeyInput().isEmpty());
    m_finish_banner->setVisible(preparedByThisWallet());
    showRequestedChange();
    refreshButtons();
}

void RotateSharedKeysDialog::refreshButtons()
{
    const bool alive{sessionAlive(m_collector, m_status_label)};
    const bool has_tx{m_collector->hasTransaction()};

    if (gateButton(m_prepare_button)) {
        const QString operator_key{operatorKeyInput()};
        const bool operator_ok{operator_key.isEmpty() || IsValidOperatorKey(operator_key)};
        const bool any_change{!operator_key.isEmpty() || !m_voting_edit->text().isEmpty()};
        const bool voting_ok{m_voting_edit->text().isEmpty() || m_voting_edit->isValid()};
        m_prepare_button->setEnabled(alive && !has_tx && any_change && operator_ok && voting_ok &&
                                     !m_fee_source->selectedAddress().isEmpty());
    }
    if (gateButton(m_approve_button)) {
        m_approve_button->setEnabled(alive && has_tx && holdsAnyShareKey() && !m_collector->complete());
        m_approve_button->setToolTip(!holdsAnyShareKey() ? NoShareKeysMessage()
                                     : !has_tx          ? tr("Prepare or paste a request first.")
                                                        : QString{});
    }
    const bool ours{preparedByThisWallet()};
    if (gateButton(m_send_button)) {
        // Combining re-signs the fee inputs, which only the preparing wallet can do
        m_send_button->setEnabled(alive && ours && m_collector->complete());
    }
    ShowReason(m_send_button, m_reason_label,
               !ours ? tr("Only the wallet that prepared the request can send it.")
                     : tr("Every share must approve first (%1 of %2 so far).")
                           .arg(m_collector->signedCount())
                           .arg(m_shares.size()));
}

void RotateSharedKeysDialog::prepare()
{
    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    // Empty strings keep the current values
    params.pushKV("operatorPubKey", operatorKeyInput().toStdString());
    params.pushKV("votingAddress", m_voting_edit->text().trimmed().toStdString());
    params.pushKV("feeSourceAddress", m_fee_source->selectedAddress().toStdString());

    ProTxResult result;
    QString error;
    if (!runCommand(shared_mn_rpc::UPDATE_REGISTRAR_PREPARE, params, result, error)) {
        ShowStatusLabel(m_status_label, error, /*error=*/true);
        return;
    }

    if (!adoptPreparedTx(m_collector, result, m_status_label)) return;

    // Read-only rather than disabled: a greyed-out field cannot be read or
    // copied, and the key that was just requested is exactly what the operator
    // has to be given next. Grouped, so 96 hex characters can be checked.
    if (const QString key{operatorKeyInput()}; !key.isEmpty()) {
        m_operator_edit->setText(MasternodeWidgetUtil::chunked(key));
        m_operator_edit->setCursorPosition(0);
    }
    m_operator_edit->setReadOnly(true);
    m_voting_edit->setReadOnly(true);
    m_fee_source->setEnabled(false);
    m_prepare_button->setEnabled(false);

    // Approving with our own share owner keys needs no decision from the user
    signWithWallet(m_collector, m_status_label);
    validateForm();
}

void RotateSharedKeysDialog::approveAndCopy()
{
    if (!signWithWallet(m_collector, m_status_label)) return;
    m_collector->copyEnvelope();
    validateForm();
}

void RotateSharedKeysDialog::sendRotation()
{
    if (!combineAndSubmit(m_collector, m_status_label, m_result_edit, tr("The key rotation was sent."),
                          tr("If the error mentions signing the transaction inputs, this is not the wallet that "
                             "prepared the request — finish on that wallet."))) {
        return;
    }
    m_approve_button->setEnabled(false);
    m_send_button->setEnabled(false);
}
