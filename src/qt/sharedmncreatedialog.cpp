// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sharedmncreatedialog.h>

#include <chainparams.h>
#include <coins.h>
#include <core_io.h>
#include <evo/dmn_types.h>
#include <evo/providertx.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <uint256.h>
#include <univalue.h>
#include <wallet/ismine.h>

#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/optionsmodel.h>
#include <qt/protxsender.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sharedmnrpc.h>
#include <qt/sharedmnwidgets.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QSpacerItem>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace {
using MasternodeWidgetUtil::CARD_PADDING;
using MasternodeWidgetUtil::GROUP_SPACING;
using MasternodeWidgetUtil::ROW_SPACING;
using MasternodeWidgetUtil::TITLE_SPACING;
using MasternodeWidgetUtil::makeCard;

//! Point size of a page heading, as in RegisterMasternodeWizard
constexpr double PAGE_TITLE_SIZE{14};

//! Default network fee the coordinator adds on top of its own share. 0.001
//! DASH covers even a maximal eight-share registration.
constexpr CAmount DEFAULT_NETWORK_FEE{100000};

//! Largest excess of funding inputs over collateral plus change accepted at
//! lock time without a warning; the excess is paid to miners as the fee.
constexpr CAmount MAX_EXPECTED_FUNDING_FEE{COIN / 100};

//! Width the broadcast confirmation is stretched to, so a grouped amount is
//! never broken across lines
constexpr int CONFIRM_MINIMUM_WIDTH{460};

//! Width of a standalone amount field. BitcoinAmountField centres its editor
//! inside whatever width a layout hands it, so it needs one of its own.
constexpr int AMOUNT_FIELD_WIDTH{300};

QLabel* MakeTitle(const QString& text, QWidget* parent)
{
    return MasternodeWidgetUtil::makeTitle(text, parent, PAGE_TITLE_SIZE);
}

//! Heading of one block inside a page, in the page's own text size
QLabel* MakeLabel(const QString& text, QWidget* parent)
{
    return MasternodeWidgetUtil::makeTitle(text, parent);
}

QLabel* MakeHint(const QString& text, QWidget* parent)
{
    return MasternodeWidgetUtil::makeHint(text, parent);
}

//! Vertical layout of a page: the dialog supplies the margins, the page only
//! keeps the rhythm between its groups.
QVBoxLayout* MakePageLayout(QWidget* page)
{
    auto* layout{new QVBoxLayout(page)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(GROUP_SPACING);
    return layout;
}

//! One group inside a page: label, hint and controls sit closer together than
//! the groups themselves do.
QVBoxLayout* MakeBlock(QVBoxLayout* page_layout)
{
    auto* block{new QVBoxLayout()};
    block->setSpacing(TITLE_SPACING);
    page_layout->addLayout(block);
    return block;
}

//! Scroll area in the wizard's style, for pages whose content outgrows the
//! dialog. Returns the container the caller fills; its layout is already set.
QWidget* MakeScrollBody(QWidget* page, QVBoxLayout* page_layout)
{
    auto* scroll{new QScrollArea(page)};
    scroll->setObjectName(QStringLiteral("mnWizardScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // Never silently clip sideways: a bar the user can reach beats a term
    // sheet column that simply is not there
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->viewport()->setAutoFillBackground(false);
    auto* container{new QWidget(scroll)};
    auto* layout{new QVBoxLayout(container)};
    layout->setContentsMargins(0, 0, ROW_SPACING, 0);
    layout->setSpacing(GROUP_SPACING);
    scroll->setWidget(container);
    page_layout->addWidget(scroll, /*stretch=*/1);
    return container;
}

//! Card with a bold title, returning the layout its body goes into
QVBoxLayout* MakeTitledCard(QWidget* parent, QVBoxLayout* parent_layout, const QString& title)
{
    auto* card{makeCard(parent)};
    auto* box{new QVBoxLayout(card)};
    box->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
    box->setSpacing(TITLE_SPACING);
    if (!title.isEmpty()) box->addWidget(MakeLabel(title, card));
    parent_layout->addWidget(card);
    return box;
}

//! A rich-text view of a term sheet: every value in it is escaped by
//! SharedMnTermSheetHtml(), so participant names stay literal text.
QLabel* MakeSheetLabel(QWidget* parent)
{
    auto* label{new QLabel(parent)};
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    return label;
}

QString FormatAmount(const WalletModel* wallet_model, CAmount amount)
{
    return SharedMnFormatAmount(SharedMnDisplayUnit(wallet_model), amount);
}

//! "1 coin" / "3 coins"
QString CoinCount(size_t coins)
{
    return SharedMnPlural(static_cast<qint64>(coins), QT_TRANSLATE_NOOP("MnShareSession", "1 coin"),
                          QT_TRANSLATE_NOOP("MnShareSession", "%1 coins"));
}

QString OutpointKey(const QString& txid, uint32_t vout)
{
    return txid.toLower() + QLatin1Char(':') + QString::number(vout);
}

QString OutpointKey(const COutPoint& outpoint)
{
    return OutpointKey(QString::fromStdString(outpoint.hash.ToString()), outpoint.n);
}

//! True when a contribution's recorded inputs include `outpoint`
bool ContributesOutpoint(const std::vector<MnShareSession::Input>& inputs, const COutPoint& outpoint)
{
    return std::any_of(inputs.begin(), inputs.end(), [&outpoint](const MnShareSession::Input& input) {
        return input.vout == outpoint.n && uint256S(input.txid.toStdString()) == outpoint.hash;
    });
}

//! scriptSig presence per input of a serialized transaction, keyed by
//! lowercase "txid:vout"; an empty map when the hex does not decode
std::map<QString, bool> InputSignatureMap(const QString& tx_hex)
{
    std::map<QString, bool> ret;
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, tx_hex.toStdString())) return ret;
    for (const auto& in : tx.vin) {
        ret[OutpointKey(QString::fromStdString(in.prevout.hash.ToString()), in.prevout.n)] = !in.scriptSig.empty();
    }
    return ret;
}

//! Name to show for a share: what the group called it, or its ordinal
QString ShareName(const MnShareSession& session, int index)
{
    if (index >= 0 && static_cast<size_t>(index) < session.shares().size() &&
        !session.shares()[index].label.isEmpty()) {
        return session.shares()[index].label;
    }
    return QObject::tr("Share %1 of %2").arg(index + 1).arg(session.shares().size());
}

//! Message box for text carrying untrusted content (participant names written
//! by somebody else): rendered as plain text so a crafted name cannot inject
//! markup into the warning that talks about it
int ShowPlainMessage(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text,
                     QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                     QMessageBox::StandardButton default_button = QMessageBox::NoButton)
{
    QMessageBox box(parent);
    box.setIcon(icon);
    box.setWindowTitle(title);
    box.setTextFormat(Qt::PlainText);
    box.setText(text);
    box.setStandardButtons(buttons);
    if (default_button != QMessageBox::NoButton) box.setDefaultButton(default_button);
    return box.exec();
}
} // anonymous namespace

//! Keeps the wallet unlocked on the GUI thread while an RPC command is in
//! flight. UnlockContext is neither copyable nor movable, so it is constructed
//! in place from requestUnlock()'s prvalue.
namespace {
struct UnlockHolder {
    WalletModel::UnlockContext ctx;
    explicit UnlockHolder(WalletModel& wallet_model) : ctx(wallet_model.requestUnlock()) {}
};
} // anonymous namespace

SharedMnCreateDialog::SharedMnCreateDialog(interfaces::Node& node, WalletModel* wallet_model, QWidget* parent) :
    QDialog(parent),
    m_node{node},
    m_wallet_model{wallet_model},
    m_v24_active{node.isV24Active()},
    m_sender{new ProTxSender(node, this)}
{
    setObjectName(QStringLiteral("SharedMnCreateDialog"));
    setWindowTitle(tr("Shared Masternode"));

    m_pages = new QStackedWidget(this);
    m_pages->insertWidget(PageLanding, createLandingPage());
    m_pages->insertWidget(PageParticipants, createParticipantsPage());
    m_pages->insertWidget(PageSettings, createSettingsPage());
    m_pages->insertWidget(PageExitTerms, createExitTermsPage());
    m_pages->insertWidget(PageContribution, createContributionPage());
    m_pages->insertWidget(PageSecret, createSecretPage());
    m_pages->insertWidget(PageInvite, createInvitePage());
    m_pages->insertWidget(PageWaitTerms, createWaitingPage(PageWaitTerms));
    m_pages->insertWidget(PageApprovals, createApprovalsPage());
    m_pages->insertWidget(PageWaitSigning, createWaitingPage(PageWaitSigning));
    m_pages->insertWidget(PageSignatures, createSignaturesPage());
    m_pages->insertWidget(PageWaitBroadcast, createWaitingPage(PageWaitBroadcast));
    m_pages->insertWidget(PageComplete, createCompletePage());

    m_progress_label = MakeHint(QString(), this);
    m_error_label = new QLabel(this);
    m_error_label->setWordWrap(true);
    m_error_label->setTextFormat(Qt::PlainText);
    m_error_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
    // Both lines only exist when they have something to say; reserving room for
    // them on every page costs the scroll body two lines it needs more
    m_error_label->setVisible(false);
    m_status_label = MakeHint(QString(), this);
    m_status_label->setTextFormat(Qt::PlainText);
    m_status_label->setVisible(false);

    m_busy_bar = new QProgressBar(this);
    m_busy_bar->setRange(0, 0);
    m_busy_bar->setTextVisible(false);
    m_busy_bar->setMaximumHeight(4);
    m_busy_bar->setVisible(false);

    m_back_button = new QPushButton(tr("Back"), this);
    m_paste_button = new QPushButton(tr("Paste"), this);
    m_save_button = new QPushButton(tr("Save…"), this);
    m_cancel_button = new QPushButton(tr("Cancel"), this);
    m_next_button = new QPushButton(tr("Next"), this);
    m_next_button->setDefault(true);
    // One filled button per page: Next. Leaving, going back, pasting and saving
    // are all alternatives to it, not competitors.
    for (QPushButton* const button : {m_back_button, m_paste_button, m_save_button, m_cancel_button}) {
        SharedMnMakeSecondary(button);
    }

    auto* buttons{new QHBoxLayout()};
    buttons->addWidget(m_back_button);
    buttons->addStretch();
    buttons->addWidget(m_paste_button);
    buttons->addWidget(m_save_button);
    buttons->addWidget(m_cancel_button);
    buttons->addWidget(m_next_button);

    // The dialog owns the page margins; the pages themselves use none so the
    // two do not add up.
    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(24, 12, 24, 12);
    layout->setSpacing(GROUP_SPACING);
    layout->addWidget(m_progress_label);
    layout->addWidget(m_pages, /*stretch=*/1);
    layout->addWidget(m_busy_bar);
    layout->addWidget(m_status_label);
    layout->addWidget(m_error_label);
    layout->addLayout(buttons);

    connect(m_back_button, &QPushButton::clicked, this, &SharedMnCreateDialog::onBack);
    connect(m_next_button, &QPushButton::clicked, this, &SharedMnCreateDialog::onNext);
    connect(m_paste_button, &QPushButton::clicked, this, &SharedMnCreateDialog::onPaste);
    connect(m_save_button, &QPushButton::clicked, this, &SharedMnCreateDialog::onSave);
    connect(m_cancel_button, &QPushButton::clicked, this, &SharedMnCreateDialog::reject);

    const auto edited = [this] { onPageEdited(); };
    for (QLineEdit* const edit : {m_service_edit, m_node_run_by_edit, static_cast<QLineEdit*>(m_voting_edit),
                                  static_cast<QLineEdit*>(m_owner_edit), static_cast<QLineEdit*>(m_refund_edit),
                                  static_cast<QLineEdit*>(m_reward_edit), m_confirm_edit}) {
        connect(edit, &QLineEdit::textChanged, this, edited);
    }
    connect(m_operator_widget, &OperatorKeyWidget::changed, this, [this] {
        rebuildOrder();
        onPageEdited();
    });

    const auto unit{SharedMnDisplayUnit(m_wallet_model)};
    for (BitcoinAmountField* const field : {m_early_penalty_field, m_fee_field}) {
        field->setDisplayUnit(unit);
    }
    for (const auto& [board_page, board] : m_boards) {
        board->setDisplayUnit(unit);
        board->setColumns({tr("Details"), tr("Funded"), tr("Approved"), tr("Signed")});
    }

    rebuildOrder();
    enterPage(PageLanding);

    GUIUtil::disableMacFocusRect(this);
    GUIUtil::updateFonts();
    SharedMnFitWrappedLabels(this);
    setMinimumSize(700, 560);
    resize(900, 740);
}

SharedMnCreateDialog::~SharedMnCreateDialog()
{
    for (QLineEdit* const edit : {m_secret_edit, m_conf_line_edit, m_confirm_edit}) {
        edit->setText(QString(edit->text().size(), QLatin1Char('0')));
        edit->clear();
    }
}

QWidget* SharedMnCreateDialog::createLandingPage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    layout->addWidget(MakeTitle(tr("Shared masternode"), page));
    layout->addWidget(MakeHint(tr("Several people fund one masternode together. One person coordinates; everyone else "
                                  "pastes what they receive and copies back a reply. Nothing reaches the network "
                                  "until the final broadcast."),
                               page));

    // Why the flow cannot start is a sentence on the page, not a tooltip on a
    // button the user cannot see the state of.
    m_landing_gate = MakeHint(QString(), page);
    m_landing_gate->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    m_landing_gate->setVisible(false);
    layout->addWidget(m_landing_gate);

    {
        auto* box{MakeTitledCard(page, layout, tr("Start"))};
        auto* card{box->parentWidget()};
        box->addWidget(MakeHint(tr("You will enter who takes part and the masternode settings, then invite the "
                                   "others."),
                                card));
        auto* row{new QHBoxLayout()};
        m_start_button = new QPushButton(tr("Start a Shared Masternode"), card);
        connect(m_start_button, &QPushButton::clicked, this, &SharedMnCreateDialog::startSession);
        row->addWidget(m_start_button);
        row->addStretch();
        box->addLayout(row);
    }

    {
        auto* box{MakeTitledCard(page, layout, tr("Continue"))};
        auto* card{box->parentWidget()};
        box->addWidget(MakeHint(tr("Paste an invitation, locked terms, a signing request or any reply you received — "
                                   "the app will know what to do."),
                                card));
        auto* row{new QHBoxLayout()};
        auto* paste{new QPushButton(tr("Paste From Clipboard"), card)};
        connect(paste, &QPushButton::clicked, this, &SharedMnCreateDialog::onPaste);
        row->addWidget(paste);
        auto* open{new QPushButton(tr("Open File…"), card)};
        SharedMnMakeSecondary(open);
        connect(open, &QPushButton::clicked, this, &SharedMnCreateDialog::onOpenFile);
        row->addWidget(open);
        row->addStretch();
        box->addLayout(row);
    }
    layout->addStretch();
    return page;
}

QWidget* SharedMnCreateDialog::createParticipantsPage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    layout->addWidget(MakeTitle(tr("Participants"), page));
    layout->addWidget(MakeHint(tr("Who takes part, and how much each contributes. Amounts must add up to exactly %1; "
                                  "every share is at least %2.")
                                   .arg(FormatAmount(m_wallet_model, GetMnType(MnType::Regular).collat_amount),
                                        FormatAmount(m_wallet_model, CCollateralShare::MIN_AMOUNT)),
                               page));

    m_share_table = new QTableWidget(0, SHARE_COLUMNS, page);
    m_share_table->setHorizontalHeaderLabels({tr("#"), tr("Name"), tr("Amount"), tr("Me")});
    m_share_table->verticalHeader()->setVisible(false);
    m_share_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_share_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QHeaderView* header{m_share_table->horizontalHeader()};
    header->setSectionResizeMode(SHARE_COL_NUMBER, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SHARE_COL_NAME, QHeaderView::Stretch);
    header->setSectionResizeMode(SHARE_COL_AMOUNT, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SHARE_COL_ME, QHeaderView::ResizeToContents);
    layout->addWidget(m_share_table, /*stretch=*/1);

    m_me_group = new QButtonGroup(page);
    m_me_group->setExclusive(true);

    auto* row{new QHBoxLayout()};
    m_add_share_button = new QPushButton(tr("Add Participant"), page);
    connect(m_add_share_button, &QPushButton::clicked, this, [this] {
        syncSharesFromTable();
        auto& shares{m_session.shares()};
        if (shares.size() >= CProRegTx::MAX_SHARES) return;
        shares.emplace_back();
        shares.back().label = tr("Participant %1").arg(shares.size());
        m_session.noteDraftChange();
        refreshShareTable();
        onPageEdited();
    });
    row->addWidget(m_add_share_button);
    m_remove_share_button = new QPushButton(tr("Remove"), page);
    connect(m_remove_share_button, &QPushButton::clicked, this, [this] {
        const int selected{m_share_table->currentRow()};
        syncSharesFromTable();
        auto& shares{m_session.shares()};
        if (selected < 0 || static_cast<size_t>(selected) >= shares.size() || shares.size() <= CProRegTx::MIN_SHARES) {
            return;
        }
        shares.erase(shares.begin() + selected);
        if (m_my_share == selected) {
            m_my_share = -1;
        } else if (m_my_share > selected) {
            --m_my_share;
        }
        m_session.noteDraftChange();
        refreshShareTable();
        onPageEdited();
    });
    row->addWidget(m_remove_share_button);
    row->addStretch();
    for (QPushButton* const button : {m_add_share_button, m_remove_share_button}) {
        SharedMnMakeSecondary(button);
    }
    m_sum_label = MakeHint(QString(), page);
    // The meter is one short reading; wrapping it leaves the tick alone on a
    // second line where it reads as a stray glyph
    m_sum_label->setWordWrap(false);
    row->addWidget(m_sum_label);
    layout->addLayout(row);
    return page;
}

QWidget* SharedMnCreateDialog::createSettingsPage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    layout->addWidget(MakeTitle(tr("Masternode settings"), page));
    layout->addWidget(MakeHint(tr("These apply to the whole masternode and are the same for every participant."),
                               page));

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};

    {
        auto* block{MakeBlock(body_layout)};
        block->addWidget(MakeLabel(tr("Service addresses:"), body));
        block->addWidget(MakeHint(tr("Public addresses the masternode serves the Core P2P network on, separated by "
                                     "commas or spaces. May be left empty and set later with a service update."),
                                  body));
        m_service_edit = new QLineEdit(body);
        m_service_edit->setPlaceholderText(QStringLiteral("1.2.3.4:%1").arg(Params().GetDefaultPort()));
        block->addWidget(m_service_edit);
    }

    {
        auto* block{MakeBlock(body_layout)};
        block->addWidget(MakeLabel(tr("Operator key:"), body));
        block->addWidget(MakeHint(tr("Whoever runs the server needs the secret key. Only the public key is registered "
                                     "on-chain."),
                                  body));
        m_operator_widget = new OperatorKeyWidget(body);
        block->addWidget(m_operator_widget);
        m_operator_imported_label = MasternodeWidgetUtil::makeValue(QString(), body, /*monospace=*/true);
        m_operator_imported_label->setVisible(false);
        block->addWidget(m_operator_imported_label);
        // Whoever runs the server needs this key; without a button the only way
        // out of this page is to select the grouped text by hand
        auto* copy_row{new QHBoxLayout()};
        auto* copy_public{new QPushButton(tr("Copy Public Key"), body)};
        SharedMnMakeSecondary(copy_public);
        connect(copy_public, &QPushButton::clicked, this, [this] {
            const QString key{m_operator_key_from_import ? m_session.terms().operatorPubKey
                                                         : m_operator_widget->publicKeyHex()};
            if (!key.isEmpty()) GUIUtil::setClipboard(key);
        });
        copy_row->addWidget(copy_public);
        copy_row->addStretch();
        block->addLayout(copy_row);
    }

    {
        auto* block{MakeBlock(body_layout)};
        block->addWidget(MakeLabel(tr("Node run by:"), body));
        block->addWidget(MakeHint(tr("Recorded in the terms so everyone knows who can start and revive the node."),
                                  body));
        m_node_run_by_edit = new QLineEdit(body);
        m_node_run_by_edit->setPlaceholderText(tr("Name of the participant who keeps the operator secret"));
        block->addWidget(m_node_run_by_edit);
    }

    {
        auto* block{MakeBlock(body_layout)};
        block->addWidget(MakeLabel(tr("Voting address:"), body));
        block->addWidget(MakeHint(tr("Votes on governance proposals for the whole masternode (P2PKH)."), body));
        auto* row{new QHBoxLayout()};
        m_voting_edit = new QValidatedLineEdit(body);
        GUIUtil::setupAddressWidget(m_voting_edit, this);
        row->addWidget(m_voting_edit, /*stretch=*/1);
        auto* fresh{new QPushButton(tr("Use new address"), body)};
        SharedMnMakeSecondary(fresh);
        connect(fresh, &QPushButton::clicked, this, [this] {
            QString error;
            const QString address{freshAddress(error)};
            if (address.isEmpty()) {
                showError(error);
            } else {
                m_voting_edit->setText(address);
            }
        });
        row->addWidget(fresh);
        block->addLayout(row);
    }

    {
        auto* block{MakeBlock(body_layout)};
        block->addWidget(MakeLabel(tr("Operator reward:"), body));
        block->addWidget(MakeHint(tr("Share of the reward promised to whoever runs the server."), body));
        m_operator_reward_spin = new QDoubleSpinBox(body);
        m_operator_reward_spin->setRange(0.0, 100.0);
        m_operator_reward_spin->setDecimals(2);
        m_operator_reward_spin->setSuffix(QString::fromUtf8(" %"));
        m_operator_reward_spin->setMaximumWidth(160);
        auto* row{new QHBoxLayout()};
        row->addWidget(m_operator_reward_spin);
        row->addStretch();
        block->addLayout(row);
        m_operator_reward_warning = MakeHint(tr("The operator will permanently receive this share of all rewards "
                                                "before anything is split between the participants."),
                                             body);
        m_operator_reward_warning->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
        m_operator_reward_warning->setVisible(false);
        block->addWidget(m_operator_reward_warning);
        connect(m_operator_reward_spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
            m_operator_reward_warning->setVisible(v > 0.0);
            onPageEdited();
        });
    }
    body_layout->addStretch();
    return page;
}

QWidget* SharedMnCreateDialog::createExitTermsPage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    layout->addWidget(MakeTitle(tr("Exit terms"), page));
    layout->addWidget(MakeHint(tr("What happens if one participant wants out before the others."), page));

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};

    {
        auto* box{MakeTitledCard(body, body_layout, tr("How leaving works"))};
        auto* card{box->parentWidget()};
        box->addWidget(MakeHint(tr("Anyone can dissolve the masternode on their own at any time, and everyone gets "
                                   "their principal back. During the early period, whoever dissolves alone pays the "
                                   "early-exit penalty out of their own share and it is split among the others. If "
                                   "everyone agrees, dissolving is free at any time."),
                                card));
    }

    {
        auto* block{MakeBlock(body_layout)};
        block->addWidget(MakeLabel(tr("Early period:"), body));
        block->addWidget(MakeHint(tr("Blocks after registration during which leaving alone costs the penalty."), body));
        auto* row{new QHBoxLayout()};
        m_early_period_spin = new QSpinBox(body);
        m_early_period_spin->setRange(0, static_cast<int>(CProRegTx::MAX_EARLY_PERIOD_BLOCKS));
        m_early_period_spin->setMaximumWidth(160);
        connect(m_early_period_spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
            refreshExitTerms();
            onPageEdited();
        });
        row->addWidget(m_early_period_spin);
        m_early_period_hint = MakeHint(QString(), body);
        row->addWidget(m_early_period_hint, /*stretch=*/1);
        block->addLayout(row);

        auto* presets{new QHBoxLayout()};
        presets->setSpacing(ROW_SPACING);
        // 2.5 minutes per block: 576 blocks a day.
        const std::pair<QString, int> options[]{
            {tr("None"), 0},      {tr("3 months"), 52560},  {tr("6 months"), 105120},
            {tr("1 year"), 210240}, {tr("2 years"), 420480},
        };
        // Shortcuts for the field above, not five separate primary actions:
        // one segmented row in which only the chosen period is filled in
        m_preset_group = new QButtonGroup(body);
        m_preset_group->setExclusive(true);
        for (const auto& [text, blocks] : options) {
            auto* button{new QPushButton(text, body)};
            button->setCheckable(true);
            SharedMnMakeSecondary(button);
            m_preset_group->addButton(button, blocks);
            connect(button, &QPushButton::clicked, this, [this, blocks] { m_early_period_spin->setValue(blocks); });
            presets->addWidget(button);
        }
        presets->addStretch();
        block->addLayout(presets);
    }

    {
        auto* block{MakeBlock(body_layout)};
        block->addWidget(MakeLabel(tr("Early-exit penalty:"), body));
        m_early_penalty_hint = MakeHint(QString(), body);
        block->addWidget(m_early_penalty_hint);
        m_early_penalty_field = new BitcoinAmountField(body);
        // Without a width of its own the field is centred inside whatever the
        // row hands it, which indents it out of line with every other control
        m_early_penalty_field->setMaximumWidth(AMOUNT_FIELD_WIDTH);
        connect(m_early_penalty_field, &BitcoinAmountField::valueChanged, this, [this] {
            refreshExitTerms();
            onPageEdited();
        });
        auto* row{new QHBoxLayout()};
        row->addWidget(m_early_penalty_field);
        row->addStretch();
        block->addLayout(row);
        m_early_preview = MakeHint(QString(), body);
        block->addWidget(m_early_preview);
        m_early_warning = MakeHint(QString(), body);
        m_early_warning->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
        m_early_warning->setVisible(false);
        block->addWidget(m_early_warning);
    }
    body_layout->addStretch();
    return page;
}

QWidget* SharedMnCreateDialog::createContributionPage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    m_contribution_title = MakeTitle(tr("Your contribution"), page);
    layout->addWidget(m_contribution_title);
    layout->addWidget(MakeHint(tr("Addresses and coins for your share. Only you see the coins; the others only see "
                                  "the resulting transaction inputs."),
                               page));

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};

    {
        auto* box{MakeTitledCard(body, body_layout, tr("Your share"))};
        auto* card{box->parentWidget()};
        m_contribution_you = MakeHint(QString(), card);
        box->addWidget(m_contribution_you);

        m_who_am_i_row = new QWidget(card);
        auto* who_layout{new QHBoxLayout(m_who_am_i_row)};
        who_layout->setContentsMargins(0, 0, 0, 0);
        who_layout->addWidget(new QLabel(tr("This is me:"), m_who_am_i_row));
        m_who_am_i_combo = new QComboBox(m_who_am_i_row);
        connect(m_who_am_i_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            if (m_updating || index < 0) return;
            m_my_share = m_who_am_i_combo->itemData(index).toInt();
            refreshContributionPage();
            onPageEdited();
        });
        who_layout->addWidget(m_who_am_i_combo, /*stretch=*/1);
        box->addWidget(m_who_am_i_row);

        const auto address_row = [this, card, box](const QString& label, const QString& hint,
                                                   QValidatedLineEdit*& edit) {
            box->addWidget(MakeLabel(label, card));
            box->addWidget(MakeHint(hint, card));
            auto* row{new QHBoxLayout()};
            edit = new QValidatedLineEdit(card);
            GUIUtil::setupAddressWidget(edit, this);
            row->addWidget(edit, /*stretch=*/1);
            auto* fresh{new QPushButton(tr("Use new address"), card)};
            SharedMnMakeSecondary(fresh);
            QValidatedLineEdit* const target{edit};
            connect(fresh, &QPushButton::clicked, this, [this, target] {
                QString error;
                const QString address{freshAddress(error)};
                if (address.isEmpty()) {
                    showError(error);
                } else {
                    target->setText(address);
                }
            });
            row->addWidget(fresh);
            box->addLayout(row);
        };
        address_row(tr("Owner address:"),
                    tr("Controls your share: approves key changes and dissolutions. Kept in this wallet."),
                    m_owner_edit);
        address_row(tr("Refund address:"),
                    tr("Receives your principal when the masternode is dissolved. Cannot be changed later."),
                    m_refund_edit);
        address_row(tr("Reward address (optional):"),
                    tr("Receives your part of the rewards. Leave empty to use the refund address; you can change it "
                       "later."),
                    m_reward_edit);
    }

    {
        auto* box{MakeTitledCard(body, body_layout, tr("Coins"))};
        auto* card{box->parentWidget()};
        m_coins_label = MakeHint(QString(), card);
        box->addWidget(m_coins_label);

        m_fee_box = new QWidget(card);
        auto* fee_layout{new QHBoxLayout(m_fee_box)};
        fee_layout->setContentsMargins(0, 0, 0, 0);
        fee_layout->addWidget(new QLabel(tr("Network fee:"), m_fee_box));
        m_fee_field = new BitcoinAmountField(m_fee_box);
        m_fee_field->setMaximumWidth(AMOUNT_FIELD_WIDTH);
        m_fee_field->setValue(DEFAULT_NETWORK_FEE);
        connect(m_fee_field, &BitcoinAmountField::valueChanged, this, [this] {
            refreshContributionPage();
            onPageEdited();
        });
        fee_layout->addWidget(m_fee_field);
        fee_layout->addStretch();
        box->addWidget(m_fee_box);

        auto* row{new QHBoxLayout()};
        m_reserve_button = new QPushButton(tr("Reserve Coins"), card);
        SharedMnMakeSecondary(m_reserve_button);
        connect(m_reserve_button, &QPushButton::clicked, this, &SharedMnCreateDialog::reserveCoins);
        row->addWidget(m_reserve_button);
        m_release_button = new QPushButton(tr("Release"), card);
        SharedMnMakeSecondary(m_release_button);
        connect(m_release_button, &QPushButton::clicked, this, &SharedMnCreateDialog::releaseCoins);
        row->addWidget(m_release_button);
        row->addStretch();
        box->addLayout(row);
    }
    body_layout->addStretch();
    return page;
}

QWidget* SharedMnCreateDialog::createSecretPage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    layout->addWidget(MakeTitle(tr("Save operator key"), page));
    layout->addWidget(MakeHint(tr("Save this generated key before inviting anyone. It is kept nowhere else and "
                                  "cannot be recovered from the wallet."),
                               page));

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};
    auto* box{MakeTitledCard(body, body_layout, tr("Operator secret key"))};
    auto* card{box->parentWidget()};
    auto* note{MakeHint(tr("Save it now — the invitation cannot go out until you confirm it."), card)};
    note->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    box->addWidget(note);
    m_secret_edit = new QLineEdit(card);
    m_secret_edit->setReadOnly(true);
    m_secret_edit->setFont(GUIUtil::fixedPitchFont());
    auto* secret_row{new QHBoxLayout()};
    secret_row->addWidget(m_secret_edit, /*stretch=*/1);
    // The raw key is what goes into a password manager; without a button the
    // only way to get it there is to select 96 characters by hand
    auto* copy_secret{new QPushButton(tr("Copy"), card)};
    SharedMnMakeSecondary(copy_secret);
    connect(copy_secret, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_secret_edit->text()); });
    secret_row->addWidget(copy_secret);
    box->addLayout(secret_row);
    box->addWidget(MakeHint(tr("Add this line to dash.conf on the masternode server:"), card));
    auto* conf_row{new QHBoxLayout()};
    m_conf_line_edit = new QLineEdit(card);
    m_conf_line_edit->setReadOnly(true);
    m_conf_line_edit->setFont(GUIUtil::fixedPitchFont());
    conf_row->addWidget(m_conf_line_edit, /*stretch=*/1);
    auto* copy{new QPushButton(tr("Copy"), card)};
    SharedMnMakeSecondary(copy);
    connect(copy, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_conf_line_edit->text()); });
    conf_row->addWidget(copy);
    box->addLayout(conf_row);
    box->addWidget(MakeHint(tr("The invitation you are about to send contains only the public key."), card));
    box->addWidget(MakeHint(tr("Type the last 4 characters of the secret key to confirm you saved it:"), card));
    m_confirm_edit = new QLineEdit(card);
    m_confirm_edit->setMaxLength(4);
    m_confirm_edit->setMaximumWidth(120);
    box->addWidget(m_confirm_edit);
    body_layout->addStretch();
    return page;
}

SharedMnStatusBoard* SharedMnCreateDialog::addBoard(Page page, QWidget* parent, QVBoxLayout* layout)
{
    auto* box{MakeTitledCard(parent, layout, tr("Participants"))};
    auto* board{new SharedMnStatusBoard(box->parentWidget())};
    box->addWidget(board);
    m_boards.emplace_back(page, board);
    return board;
}

QWidget* SharedMnCreateDialog::createInvitePage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    layout->addWidget(MakeTitle(tr("Invite participants"), page));
    layout->addWidget(MakeHint(tr("Send the same invitation to everyone. Each person reserves their coins and sends "
                                  "you back their details."),
                               page));

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};
    addBoard(PageInvite, body, body_layout);

    auto* row{new QHBoxLayout()};
    m_copy_invitation_button = new QPushButton(tr("Copy Invitation"), body);
    connect(m_copy_invitation_button, &QPushButton::clicked, this, [this] { copySession(tr("Invitation")); });
    row->addWidget(m_copy_invitation_button);
    m_save_invitation_button = new QPushButton(tr("Save Invitation…"), body);
    SharedMnMakeSecondary(m_save_invitation_button);
    connect(m_save_invitation_button, &QPushButton::clicked, this, &SharedMnCreateDialog::saveSession);
    row->addWidget(m_save_invitation_button);
    row->addStretch();
    body_layout->addLayout(row);

    m_invite_edit_warning = MakeHint(tr("You changed the draft. Participants who already replied must reply again."),
                                     body);
    m_invite_edit_warning->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    m_invite_edit_warning->setVisible(false);
    body_layout->addWidget(m_invite_edit_warning);

    // The lock confirmation replaces the page's content in place rather than
    // opening a modal: what is being locked is exactly what the page shows.
    {
        auto* box{MakeTitledCard(body, body_layout, tr("Lock these terms?"))};
        m_lock_confirm_card = box->parentWidget();
        m_lock_terms_label = MakeSheetLabel(m_lock_confirm_card);
        box->addWidget(m_lock_terms_label);
        m_lock_funding_label = MakeHint(QString(), m_lock_confirm_card);
        box->addWidget(m_lock_funding_label);
        box->addWidget(MakeHint(tr("Locking fixes every address, amount and coin. Afterwards nobody can edit — only "
                                   "unlock, which discards all approvals."),
                                m_lock_confirm_card));
        m_lock_confirm_card->setVisible(false);
    }
    body_layout->addStretch();
    return page;
}

QWidget* SharedMnCreateDialog::createApprovalsPage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    m_approvals_title = MakeTitle(tr("Approvals"), page);
    layout->addWidget(m_approvals_title);
    m_approvals_purpose = MakeHint(QString(), page);
    layout->addWidget(m_approvals_purpose);

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};

    m_prepare_warning_label = MakeHint(QString(), body);
    m_prepare_warning_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    m_prepare_warning_label->setVisible(false);
    body_layout->addWidget(m_prepare_warning_label);

    addBoard(PageApprovals, body, body_layout);

    {
        auto* box{MakeTitledCard(body, body_layout, QString())};
        m_approvals_terms = MakeSheetLabel(box->parentWidget());
        box->addWidget(m_approvals_terms);
    }

    auto* row{new QHBoxLayout()};
    m_copy_terms_button = new QPushButton(tr("Copy Locked Terms"), body);
    connect(m_copy_terms_button, &QPushButton::clicked, this, [this] { copySession(tr("Locked Terms")); });
    row->addWidget(m_copy_terms_button);
    m_unlock_button = new QPushButton(tr("Unlock terms…"), body);
    SharedMnMakeSecondary(m_unlock_button);
    connect(m_unlock_button, &QPushButton::clicked, this, &SharedMnCreateDialog::unlockTerms);
    row->addWidget(m_unlock_button);
    row->addStretch();
    body_layout->addLayout(row);
    body_layout->addStretch();
    return page;
}

QWidget* SharedMnCreateDialog::createSignaturesPage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    m_signatures_title = MakeTitle(tr("Signatures"), page);
    layout->addWidget(m_signatures_title);
    m_signatures_purpose = MakeHint(QString(), page);
    layout->addWidget(m_signatures_purpose);

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};
    addBoard(PageSignatures, body, body_layout);

    m_signatures_summary = MakeHint(QString(), body);
    body_layout->addWidget(m_signatures_summary);

    auto* row{new QHBoxLayout()};
    m_copy_signing_button = new QPushButton(tr("Copy Signing Request"), body);
    connect(m_copy_signing_button, &QPushButton::clicked, this, [this] { copySession(tr("Signing Request")); });
    row->addWidget(m_copy_signing_button);
    row->addStretch();
    body_layout->addLayout(row);
    body_layout->addStretch();
    return page;
}

QWidget* SharedMnCreateDialog::createWaitingPage(Page page_id)
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    WaitingPage widgets;
    widgets.title = MakeTitle(QString(), page);
    layout->addWidget(widgets.title);

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};

    {
        auto* box{MakeTitledCard(body, body_layout, tr("What you sent"))};
        widgets.sent = MakeHint(QString(), box->parentWidget());
        widgets.sent->setTextFormat(Qt::PlainText);
        box->addWidget(widgets.sent);
    }
    {
        auto* box{MakeTitledCard(body, body_layout, tr("What comes next"))};
        auto* card{box->parentWidget()};
        widgets.next = MakeHint(QString(), card);
        box->addWidget(widgets.next);
        auto* row{new QHBoxLayout()};
        auto* paste{new QPushButton(tr("Paste From Clipboard"), card)};
        connect(paste, &QPushButton::clicked, this, &SharedMnCreateDialog::onPaste);
        row->addWidget(paste);
        auto* open{new QPushButton(tr("Open File…"), card)};
        SharedMnMakeSecondary(open);
        connect(open, &QPushButton::clicked, this, &SharedMnCreateDialog::onOpenFile);
        row->addWidget(open);
        row->addStretch();
        box->addLayout(row);

        // Waiting can last days, and the coordinator may never come back. The
        // reservation survives restarts, so without this the only way out is
        // Coin Control.
        widgets.release = new QPushButton(tr("Release my coins"), card);
        widgets.release->setFlat(true);
        connect(widgets.release, &QPushButton::clicked, this, &SharedMnCreateDialog::releaseReservedCoins);
        box->addWidget(widgets.release, 0, Qt::AlignLeft);
    }
    if (page_id != PageWaitTerms) addBoard(page_id, body, body_layout);
    body_layout->addStretch();
    m_waiting_pages.emplace_back(page_id, widgets);
    return page;
}

QWidget* SharedMnCreateDialog::createCompletePage()
{
    auto* page{new QWidget(this)};
    auto* layout{MakePageLayout(page)};
    layout->addWidget(MakeTitle(tr("Shared masternode registered"), page));

    auto* body{MakeScrollBody(page, layout)};
    auto* body_layout{qobject_cast<QVBoxLayout*>(body->layout())};

    {
        auto* box{MakeTitledCard(body, body_layout, tr("What happened"))};
        auto* card{box->parentWidget()};
        m_complete_hash = MasternodeWidgetUtil::makeValue(QString(), card, /*monospace=*/true);
        box->addWidget(m_complete_hash);
        auto* copy_row{new QHBoxLayout()};
        auto* copy_hash{new QPushButton(tr("Copy"), card)};
        SharedMnMakeSecondary(copy_hash);
        connect(copy_hash, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_complete_hash->text()); });
        copy_row->addWidget(copy_hash);
        copy_row->addStretch();
        box->addLayout(copy_row);
        box->addWidget(MakeHint(tr("It appears under Masternodes after one confirmation."), card));
    }

    {
        auto* box{MakeTitledCard(body, body_layout, tr("What to keep safe"))};
        auto* card{box->parentWidget()};
        m_keep_owner = new QCheckBox(tr("Your share owner key"), card);
        box->addWidget(m_keep_owner);
        m_keep_owner_hint = MakeHint(QString(), card);
        m_keep_owner_hint->setTextFormat(Qt::PlainText);
        box->addWidget(m_keep_owner_hint);

        m_keep_operator = new QCheckBox(tr("Operator secret key"), card);
        box->addWidget(m_keep_operator);
        m_keep_operator_hint = MakeHint(QString(), card);
        m_keep_operator_hint->setTextFormat(Qt::PlainText);
        box->addWidget(m_keep_operator_hint);

        m_keep_standby = new QCheckBox(tr("Standby dissolution"), card);
        box->addWidget(m_keep_standby);
        box->addWidget(MakeHint(tr("A signed dissolution you keep offline. It recovers your principal even if this "
                                   "wallet is lost. Create it after the first confirmation."),
                                card));
        box->addWidget(MakeHint(tr("Create it from the Masternodes list (right-click the masternode) once the "
                                   "registration is confirmed, and store it with your refund-address backup, "
                                   "separately from this wallet."),
                                card));

        m_keep_record = new QCheckBox(tr("Session record"), card);
        box->addWidget(m_keep_record);
        auto* record_row{new QHBoxLayout()};
        auto* save_record{new QPushButton(tr("Save Final Record…"), card)};
        SharedMnMakeSecondary(save_record);
        connect(save_record, &QPushButton::clicked, this, &SharedMnCreateDialog::saveSession);
        record_row->addWidget(save_record);
        record_row->addStretch();
        box->addLayout(record_row);
    }

    {
        auto* box{MakeTitledCard(body, body_layout, tr("Next steps"))};
        m_next_steps = new QLabel(box->parentWidget());
        m_next_steps->setWordWrap(true);
        m_next_steps->setTextInteractionFlags(Qt::TextSelectableByMouse);
        box->addWidget(m_next_steps);
    }

    auto* row{new QHBoxLayout()};
    m_copy_record_button = new QPushButton(tr("Copy Final Record"), body);
    connect(m_copy_record_button, &QPushButton::clicked, this, [this] { copySession(tr("Final Record")); });
    row->addWidget(m_copy_record_button);
    m_complete_release_button = new QPushButton(tr("Release my coins"), body);
    m_complete_release_button->setFlat(true);
    connect(m_complete_release_button, &QPushButton::clicked, this, &SharedMnCreateDialog::releaseReservedCoins);
    row->addWidget(m_complete_release_button);
    row->addStretch();
    body_layout->addLayout(row);
    body_layout->addStretch();
    return page;
}

bool SharedMnCreateDialog::canSign() const
{
    return m_wallet_model != nullptr && !m_wallet_model->wallet().privateKeysDisabled();
}

bool SharedMnCreateDialog::secretGateRequired() const
{
    return m_operator_widget != nullptr && m_operator_widget->hasGeneratedSecret() && !m_operator_key_from_import;
}

bool SharedMnCreateDialog::secretConfirmed() const
{
    return m_confirm_edit->text().trimmed().compare(m_operator_widget->secretHex().right(4), Qt::CaseInsensitive) == 0;
}

bool SharedMnCreateDialog::operatorSecretUnsaved() const
{
    return secretGateRequired() && !secretConfirmed();
}

void SharedMnCreateDialog::rebuildOrder()
{
    const std::optional<Page> current{m_order.isEmpty() ? std::nullopt : std::optional<Page>{currentPage()}};
    m_order.clear();
    m_order << PageLanding;
    if (m_role == Role::Coordinator) {
        m_order << PageParticipants << PageSettings << PageExitTerms << PageContribution;
        if (secretGateRequired()) m_order << PageSecret;
        m_order << PageInvite << PageApprovals << PageSignatures << PageComplete;
    } else if (m_role == Role::Participant) {
        m_order << PageContribution << PageWaitTerms << PageApprovals << PageWaitSigning << PageSignatures
                << PageWaitBroadcast << PageComplete;
    }
    if (current && m_order.contains(*current)) {
        m_pos = m_order.indexOf(*current);
    } else if (m_pos >= m_order.size()) {
        m_pos = m_order.size() - 1;
    }
    updateProgress();
}

SharedMnCreateDialog::Page SharedMnCreateDialog::currentPage() const
{
    return m_order.isEmpty() ? PageLanding : m_order.at(m_pos);
}

QString SharedMnCreateDialog::pageTitle(Page page) const
{
    const bool coordinator{m_role == Role::Coordinator};
    switch (page) {
    case PageLanding:
        return tr("Shared masternode");
    case PageParticipants:
        return tr("Participants");
    case PageSettings:
        return tr("Masternode settings");
    case PageExitTerms:
        return tr("Exit terms");
    case PageContribution:
        return coordinator ? tr("Your contribution") : tr("Your share");
    case PageSecret:
        return tr("Save operator key");
    case PageInvite:
        return tr("Invite participants");
    case PageWaitTerms:
    case PageWaitSigning:
    case PageWaitBroadcast:
        return tr("Waiting for %1").arg(m_session.coordinatorLabel().isEmpty() ? tr("the coordinator")
                                                                              : m_session.coordinatorLabel());
    case PageApprovals:
        return coordinator ? tr("Approvals") : tr("Approve terms");
    case PageSignatures:
        return coordinator ? tr("Signatures") : tr("Sign contribution");
    case PageComplete:
        return tr("Complete");
    }
    return {};
}

void SharedMnCreateDialog::updateProgress()
{
    if (m_progress_label == nullptr || m_order.isEmpty()) return;
    const Page page{currentPage()};
    // Only the five pages that build the invitation are numbered; what follows
    // depends on how fast the others answer, so a count would be a promise the
    // dialog cannot keep.
    static const QVector<Page> numbered{PageParticipants, PageSettings, PageExitTerms, PageContribution, PageInvite};
    if (m_role == Role::Coordinator && numbered.contains(page)) {
        m_progress_label->setText(tr("Step %1 of %2 · %3")
                                      .arg(numbered.indexOf(page) + 1)
                                      .arg(numbered.size())
                                      .arg(pageTitle(page)));
        return;
    }
    m_progress_label->setText(pageTitle(page));
}

void SharedMnCreateDialog::goToPage(Page page)
{
    const int pos{m_order.indexOf(page)};
    if (pos < 0) return;
    m_pos = pos;
    enterPage(page);
}

void SharedMnCreateDialog::enterPage(Page page)
{
    m_pages->setCurrentIndex(page);
    m_validation_page.reset();
    // Neither line survives a page change: a confirmation about the page just
    // left is stale the moment another one is on screen
    showError(QString());
    showStatus(QString());
    if (page != PageInvite) m_lock_confirming = false;
    switch (page) {
    case PageLanding: {
        QString gate;
        if (m_wallet_model == nullptr) {
            gate = tr("No wallet is available.");
        } else if (!m_v24_active) {
            gate = tr("Shared masternodes need the v24 upgrade, which is not active on this network yet.");
        } else if (!canSign()) {
            gate = tr("This wallet cannot sign. You can view, but not take part.");
        }
        m_landing_gate->setText(gate);
        m_landing_gate->setVisible(!gate.isEmpty());
        m_start_button->setEnabled(gate.isEmpty());
        break;
    }
    case PageParticipants:
        refreshShareTable();
        break;
    case PageSettings:
        if (m_voting_edit->text().isEmpty() && m_wallet_model != nullptr) {
            QString error;
            const QString address{freshAddress(error)};
            if (!address.isEmpty()) m_voting_edit->setText(address);
        }
        break;
    case PageExitTerms:
        refreshExitTerms();
        break;
    case PageContribution:
        inferMyShare();
        // Addresses are filled in on first entry, as the register wizard does,
        // so the common case needs no typing at all.
        for (QValidatedLineEdit* const edit : {m_owner_edit, m_refund_edit}) {
            if (!edit->text().isEmpty() || m_wallet_model == nullptr) continue;
            QString error;
            const QString address{freshAddress(error)};
            if (!address.isEmpty()) edit->setText(address);
        }
        refreshContributionPage();
        break;
    case PageSecret:
        m_secret_edit->setText(m_operator_widget->secretHex());
        m_conf_line_edit->setText(QStringLiteral("masternodeblsprivkey=%1").arg(m_operator_widget->secretHex()));
        m_secret_edit->setCursorPosition(0);
        m_conf_line_edit->setCursorPosition(0);
        break;
    case PageInvite:
        refreshInvitePage();
        break;
    case PageApprovals:
        refreshApprovalsPage();
        break;
    case PageSignatures:
        refreshSignaturesPage();
        break;
    case PageWaitTerms:
    case PageWaitSigning:
    case PageWaitBroadcast:
        refreshWaitingPages();
        break;
    case PageComplete:
        refreshCompletePage();
        break;
    }
    refreshBoards();
    updateProgress();
    updateButtons();
    // The exception to the rule above: a coin of ours the session spends
    // outside our own contribution is about the session, not about the page
    // that happens to be on screen, and it must be read before anything is
    // signed
    if (!m_foreign_input.isEmpty()) showError(foreignInputError(m_foreign_input));
}

bool SharedMnCreateDialog::validatePage(Page page, QString& err)
{
    err.clear();
    switch (page) {
    case PageParticipants: {
        syncSharesFromTable();
        const auto& shares{m_session.shares()};
        const CAmount collateral{GetMnType(MnType::Regular).collat_amount};
        if (shares.size() < CProRegTx::MIN_SHARES || shares.size() > CProRegTx::MAX_SHARES) {
            err = tr("A shared masternode needs between %1 and %2 participants.")
                      .arg(CProRegTx::MIN_SHARES)
                      .arg(CProRegTx::MAX_SHARES);
            break;
        }
        CAmount total{0};
        QSet<QString> names;
        for (size_t i = 0; i < shares.size(); ++i) {
            const QString name{shares[i].label.trimmed()};
            if (name.isEmpty()) {
                err = tr("Enter a name for participant %1.").arg(i + 1);
                break;
            }
            if (names.contains(name)) {
                err = tr("The name \"%1\" is used twice.").arg(name);
                break;
            }
            names.insert(name);
            if (shares[i].amount < CCollateralShare::MIN_AMOUNT) {
                err = tr("Each share is at least %1.")
                          .arg(FormatAmount(m_wallet_model, CCollateralShare::MIN_AMOUNT));
                break;
            }
            total += shares[i].amount;
        }
        if (!err.isEmpty()) break;
        if (total != collateral) {
            err = tr("The amounts add up to %1; they must be exactly %2.")
                      .arg(FormatAmount(m_wallet_model, total), FormatAmount(m_wallet_model, collateral));
            break;
        }
        if (m_my_share < 0 || static_cast<size_t>(m_my_share) >= shares.size()) {
            err = tr("Select the participant that is you.");
        }
        break;
    }
    case PageSettings: {
        if (!m_operator_key_from_import && !m_operator_widget->isValid()) {
            err = tr("Enter a valid operator BLS public key (96 hexadecimal characters, basic scheme).");
            break;
        }
        if (!MasternodeWidgetUtil::isP2PKHAddress(m_voting_edit->text().trimmed())) {
            err = tr("Enter a valid voting address (P2PKH).");
            break;
        }
        interfaces::ProviderNetInfo net_info;
        for (const QString& entry : MasternodeWidgetUtil::tokenizeEndpointList(m_service_edit->text())) {
            net_info.core_p2p.push_back(entry.toStdString());
        }
        const uint16_t version{m_node.evo().getProviderTxCapabilities().version};
        if (const auto error{
                m_node.evo().validateProviderNetInfo(net_info, MnType::Regular, version, /*optional=*/true)}) {
            err = QString::fromStdString(error->message.translated);
        }
        break;
    }
    case PageExitTerms: {
        const CAmount penalty{m_early_penalty_field->value()};
        const uint32_t period{static_cast<uint32_t>(m_early_period_spin->value())};
        CAmount smallest{GetMnType(MnType::Regular).collat_amount};
        for (const auto& share : m_session.shares()) {
            if (share.amount > 0) smallest = std::min(smallest, share.amount);
        }
        if (period == 0 && penalty != 0) {
            err = tr("Set the early-exit penalty to 0 when there is no early period.");
        } else if (penalty >= smallest) {
            err = tr("The early-exit penalty must be less than the smallest share (%1).")
                      .arg(FormatAmount(m_wallet_model, smallest));
        }
        break;
    }
    case PageContribution: {
        if (m_my_share < 0 || static_cast<size_t>(m_my_share) >= m_session.shares().size()) {
            err = tr("Select the participant that is you.");
            break;
        }
        const QString owner{m_owner_edit->text().trimmed()};
        const QString refund{m_refund_edit->text().trimmed()};
        const QString reward{m_reward_edit->text().trimmed()};
        if (!MasternodeWidgetUtil::isP2PKHAddress(owner)) {
            err = tr("Enter a valid owner address (P2PKH).");
            break;
        }
        if (!MasternodeWidgetUtil::isP2PKHorP2SHAddress(refund)) {
            err = tr("Enter a valid refund address (P2PKH or P2SH).");
            break;
        }
        if (!reward.isEmpty() && !MasternodeWidgetUtil::isP2PKHorP2SHAddress(reward)) {
            err = tr("Enter a valid reward address (P2PKH or P2SH), or leave it empty.");
            break;
        }
        if (!reward.isEmpty() &&
            (reward == m_session.terms().votingAddress || reward == owner ||
             std::any_of(m_session.shares().begin(), m_session.shares().end(),
                         [&](const auto& share) { return !share.ownerAddress.isEmpty() &&
                                                         share.ownerAddress == reward; }))) {
            err = tr("The reward address must differ from the voting address and every owner address.");
            break;
        }
        break;
    }
    case PageInvite:
        if (!allDetailsCollected()) {
            err = tr("Every participant must send back their details before the terms can be locked.");
        }
        break;
    case PageApprovals:
        if (!m_foreign_input.isEmpty()) {
            err = foreignInputError(m_foreign_input);
        } else if (m_role == Role::Participant && !canSign()) {
            err = tr("This wallet is watch-only and cannot sign.");
        }
        break;
    case PageSignatures:
        if (!m_foreign_input.isEmpty()) err = foreignInputError(m_foreign_input);
        break;
    default:
        break;
    }
    return err.isEmpty();
}

void SharedMnCreateDialog::updateButtons()
{
    const Page page{currentPage()};
    const bool coordinator{m_role == Role::Coordinator};
    const bool participant{m_role == Role::Participant};

    m_back_button->setVisible(coordinator && page != PageLanding && page != PageComplete &&
                              m_session.stage() == MnShareSession::Stage::Draft);
    m_back_button->setEnabled(!m_busy && m_pos > 0);
    m_cancel_button->setVisible(page != PageComplete);
    m_cancel_button->setEnabled(!m_busy);

    const bool exchanging{page != PageLanding && page != PageComplete && m_role != Role::Undecided};
    m_paste_button->setVisible(exchanging);
    m_paste_button->setEnabled(!m_busy);
    const bool secret_unsaved{operatorSecretUnsaved()};
    m_save_button->setVisible(exchanging);
    m_save_button->setEnabled(!m_busy && !secret_unsaved);
    m_save_button->setToolTip(secret_unsaved ? tr("Save the operator secret key first. It is shown once, on the "
                                                  "\"Save operator key\" page, and the session file does not carry "
                                                  "it.")
                                             : QString());

    QString next_text{tr("Next")};
    bool next_visible{true};
    switch (page) {
    case PageLanding:
        next_visible = false;
        break;
    case PageContribution:
        if (participant) next_text = tr("Reserve Coins and Copy Details");
        break;
    case PageSecret:
        next_text = tr("Continue");
        break;
    case PageInvite:
        next_text = m_lock_confirming ? tr("Lock and Approve") : tr("Lock Terms");
        break;
    case PageApprovals:
        if (participant) {
            next_text = tr("Approve and Copy Reply");
        } else if (needsOwnApproval()) {
            // Without it the session can never be combined, and the only other
            // way out is "Unlock terms", which discards everybody else's
            // approvals
            next_text = tr("Approve");
        } else {
            next_visible = allApproved();
            next_text = tr("Retry");
        }
        break;
    case PageSignatures:
        if (participant) {
            next_text = tr("Sign and Copy Reply");
        } else {
            next_visible = true;
            next_text = tr("Broadcast Registration");
        }
        break;
    case PageWaitTerms:
    case PageWaitSigning:
        next_visible = false;
        break;
    case PageWaitBroadcast:
        next_text = tr("Close");
        break;
    case PageComplete:
        next_text = tr("Finish");
        break;
    default:
        break;
    }
    if (!m_dead_reason.isEmpty()) {
        next_visible = true;
        next_text = tr("Start Over");
    }
    m_next_button->setVisible(next_visible);
    if (!m_busy) m_next_button->setText(next_text);

    bool enabled{!m_busy};
    QString tooltip;
    if (!m_dead_reason.isEmpty()) {
        // Start Over is the one thing left to do, so it stays clickable
    } else if (m_wallet_model == nullptr) {
        enabled = false;
        tooltip = tr("No wallet is available.");
    } else if (!canSign() && page != PageComplete && page != PageWaitBroadcast) {
        enabled = false;
        tooltip = tr("This wallet is watch-only and cannot sign.");
    } else if (!m_foreign_input.isEmpty() && (page == PageApprovals || page == PageSignatures)) {
        enabled = false;
        tooltip = foreignInputError(m_foreign_input);
    } else if (page == PageSecret && !secretConfirmed()) {
        enabled = false;
        tooltip = tr("Confirm you saved the operator secret key by typing its last 4 characters.");
    } else if (page == PageInvite && !allDetailsCollected()) {
        enabled = false;
        tooltip = tr("Every participant must send back their details before the terms can be locked.");
    } else if (page == PageApprovals && participant && !m_session.signatureFor(m_my_share).isEmpty()) {
        enabled = false;
        tooltip = tr("You already approved these terms.");
    } else if (page == PageSignatures && coordinator && !allFundingSigned()) {
        enabled = false;
        tooltip = tr("Every participant must sign their own coins before the registration can be broadcast.");
    } else if (m_validation_page && *m_validation_page == page) {
        QString error;
        if (!validatePage(page, error)) {
            enabled = false;
            tooltip = error;
        }
    }
    m_next_button->setEnabled(enabled);
    m_next_button->setToolTip(tooltip);
}

void SharedMnCreateDialog::onPageEdited()
{
    if (m_updating) return;
    const Page page{currentPage()};
    if (page == PageParticipants) {
        syncSharesFromTable();
        refreshSumMeter();
    } else if (page == PageSettings || page == PageExitTerms) {
        syncTermsToSession();
    }
    if (m_validation_page && *m_validation_page == page) {
        QString error;
        validatePage(page, error);
        showError(error);
        if (error.isEmpty()) m_validation_page.reset();
    } else if (!m_error_label->text().isEmpty()) {
        showError(QString());
    }
    updateButtons();
}

void SharedMnCreateDialog::setBusy(bool busy, const QString& busy_text)
{
    m_busy = busy;
    m_busy_bar->setVisible(busy);
    m_pages->setEnabled(!busy);
    if (busy && !busy_text.isEmpty()) m_next_button->setText(busy_text);
    updateButtons();
}

void SharedMnCreateDialog::showError(const QString& message)
{
    m_error_label->setText(message);
    m_error_label->setVisible(!message.isEmpty());
    if (!message.isEmpty()) {
        m_status_label->clear();
        m_status_label->setVisible(false);
    }
}

void SharedMnCreateDialog::showStatus(const QString& message)
{
    m_status_label->setText(message);
    m_status_label->setVisible(!message.isEmpty());
    if (!message.isEmpty()) {
        m_error_label->clear();
        m_error_label->setVisible(false);
    }
}

void SharedMnCreateDialog::onBack()
{
    if (m_busy || m_pos == 0) return;
    if (m_lock_confirming) {
        m_lock_confirming = false;
        refreshInvitePage();
        updateButtons();
        return;
    }
    --m_pos;
    enterPage(m_order[m_pos]);
}

void SharedMnCreateDialog::onNext()
{
    if (m_busy) return;
    if (!m_dead_reason.isEmpty()) {
        startSession();
        return;
    }
    const Page page{currentPage()};
    QString err;
    if (!validatePage(page, err)) {
        m_validation_page = page;
        showError(err);
        updateButtons();
        return;
    }
    m_validation_page.reset();
    showError(QString());

    switch (page) {
    case PageContribution:
        // The primary button does what it says: the addresses are validated
        // above, the coins are reserved here if they are not already, and only
        // then does the page move on.
        if (myContribution() == nullptr) {
            reserveCoins();
            if (myContribution() == nullptr) return; // reserveCoins() said why
        }
        if (m_role == Role::Participant) {
            copySession(tr("Details"));
            goToPage(PageWaitTerms);
            return;
        }
        break;
    case PageInvite:
        if (!m_lock_confirming) {
            beginLockConfirmation();
            return;
        }
        lockTerms();
        return;
    case PageApprovals:
        if (m_role == Role::Participant) {
            approveAndCopy();
        } else if (needsOwnApproval()) {
            QString sign_error;
            if (!signOwnConsent(sign_error)) {
                showError(sign_error);
                refreshAll();
                return;
            }
            refreshAll();
            if (allApproved()) runCombineAndSign();
        } else {
            runCombineAndSign();
        }
        return;
    case PageSignatures:
        if (m_role == Role::Participant) {
            signAndCopy();
        } else {
            broadcastRegistration();
        }
        return;
    case PageWaitBroadcast:
    case PageComplete:
        m_finished = true;
        accept();
        return;
    default:
        break;
    }
    if (m_pos + 1 < m_order.size()) {
        ++m_pos;
        enterPage(m_order[m_pos]);
    }
}

void SharedMnCreateDialog::refreshAll()
{
    pushSessionToWidgets();
    refreshWindowTitle();
    refreshShareTable();
    refreshExitTerms();
    refreshContributionPage();
    refreshInvitePage();
    refreshApprovalsPage();
    refreshSignaturesPage();
    refreshWaitingPages();
    refreshCompletePage();
    refreshBoards();
    updateProgress();
    updateButtons();
    if (!m_dead_reason.isEmpty()) {
        showError(m_dead_reason);
    } else if (!m_foreign_input.isEmpty()) {
        showError(foreignInputError(m_foreign_input));
    }
}

void SharedMnCreateDialog::refreshWindowTitle()
{
    setWindowTitle(m_role == Role::Undecided ? tr("Shared Masternode")
                                             : tr("Shared Masternode — Session %1").arg(m_session.sessionCode()));
}

void SharedMnCreateDialog::refreshShareTable()
{
    m_updating = true;
    const auto& shares{m_session.shares()};
    const int rows{static_cast<int>(shares.size())};
    const bool editable{m_session.stage() == MnShareSession::Stage::Draft};
    if (m_share_table->rowCount() != rows) {
        // Rebuilding drops the row's widgets, so only do it when rows moved
        m_share_table->setRowCount(0);
        m_share_table->setRowCount(rows);
        for (int row = 0; row < rows; ++row) {
            auto* number{new QTableWidgetItem(QString::number(row + 1))};
            number->setTextAlignment(Qt::AlignCenter);
            m_share_table->setItem(row, SHARE_COL_NUMBER, number);

            auto* name{new QLineEdit(m_share_table)};
            name->setPlaceholderText(tr("Name"));
            connect(name, &QLineEdit::textChanged, this, [this] { onPageEdited(); });
            m_share_table->setCellWidget(row, SHARE_COL_NAME, name);

            // A real amount field rather than a text cell: a typo must show as
            // an invalid field, never quietly become a zero share.
            auto* amount{new BitcoinAmountField(m_share_table)};
            amount->setDisplayUnit(SharedMnDisplayUnit(m_wallet_model));
            connect(amount, &BitcoinAmountField::valueChanged, this, [this] { onPageEdited(); });
            m_share_table->setCellWidget(row, SHARE_COL_AMOUNT, amount);

            auto* me_holder{new QWidget(m_share_table)};
            auto* me_layout{new QHBoxLayout(me_holder)};
            me_layout->setContentsMargins(0, 0, 0, 0);
            me_layout->addStretch();
            auto* me{new QRadioButton(me_holder)};
            m_me_group->addButton(me, row);
            me_layout->addWidget(me);
            me_layout->addStretch();
            m_share_table->setCellWidget(row, SHARE_COL_ME, me_holder);
        }
        m_share_table->resizeRowsToContents();
    }
    for (int row = 0; row < rows; ++row) {
        if (auto* name = qobject_cast<QLineEdit*>(m_share_table->cellWidget(row, SHARE_COL_NAME))) {
            if (name->text() != shares[row].label) name->setText(shares[row].label);
            name->setReadOnly(!editable);
        }
        if (auto* amount = qobject_cast<BitcoinAmountField*>(m_share_table->cellWidget(row, SHARE_COL_AMOUNT))) {
            if (amount->value() != shares[row].amount) amount->setValue(shares[row].amount);
            amount->setEnabled(editable);
        }
        if (auto* me = qobject_cast<QRadioButton*>(m_me_group->button(row))) {
            me->setChecked(row == m_my_share);
            me->setEnabled(editable);
        }
    }
    m_add_share_button->setEnabled(editable && shares.size() < CProRegTx::MAX_SHARES);
    m_remove_share_button->setEnabled(editable && shares.size() > CProRegTx::MIN_SHARES);
    m_updating = false;
    refreshSumMeter();
}

void SharedMnCreateDialog::refreshSumMeter()
{
    const CAmount collateral{GetMnType(MnType::Regular).collat_amount};
    CAmount total{0};
    for (const auto& share : m_session.shares()) {
        total += share.amount;
    }
    const QString have{FormatAmount(m_wallet_model, total)};
    const QString want{FormatAmount(m_wallet_model, collateral)};
    if (total == collateral) {
        m_sum_label->setText(tr("%1 of %2%3✓").arg(have, want, QChar(QChar::Nbsp)));
    } else if (total < collateral) {
        m_sum_label->setText(
            tr("%1 of %2 — %3 missing").arg(have, want, FormatAmount(m_wallet_model, collateral - total)));
    } else {
        m_sum_label->setText(
            tr("%1 of %2 — %3 too much").arg(have, want, FormatAmount(m_wallet_model, total - collateral)));
    }
}

void SharedMnCreateDialog::refreshExitTerms()
{
    const uint32_t blocks{static_cast<uint32_t>(m_early_period_spin->value())};
    m_early_period_hint->setText(MnShareSession::HumanEarlyPeriod(blocks));
    if (m_preset_group != nullptr) {
        QAbstractButton* const preset{m_preset_group->button(static_cast<int>(blocks))};
        m_preset_group->setExclusive(preset != nullptr);
        if (preset != nullptr) {
            preset->setChecked(true);
        } else if (QAbstractButton* const checked = m_preset_group->checkedButton(); checked != nullptr) {
            checked->setChecked(false);
        }
        m_preset_group->setExclusive(true);
    }

    CAmount smallest{GetMnType(MnType::Regular).collat_amount};
    int smallest_index{-1};
    const auto& shares{m_session.shares()};
    for (size_t i = 0; i < shares.size(); ++i) {
        if (shares[i].amount > 0 && shares[i].amount <= smallest) {
            smallest = shares[i].amount;
            smallest_index = static_cast<int>(i);
        }
    }
    m_early_penalty_hint->setText(tr("Must be less than the smallest share (%1). Set to 0 only if you accept that "
                                     "anyone can leave at any time for free.")
                                      .arg(FormatAmount(m_wallet_model, smallest)));

    const CAmount penalty{m_early_penalty_field->value()};
    if (blocks == 0 || penalty == 0) {
        m_early_preview->setText(tr("Anyone can leave at any time for free."));
    } else if (smallest_index >= 0) {
        m_early_preview->setText(tr("If %1 leaves before block %2 of the masternode's life, %1 forfeits %3, split "
                                    "among the others.")
                                     .arg(ShareName(m_session, smallest_index))
                                     .arg(blocks)
                                     .arg(FormatAmount(m_wallet_model, penalty)));
    } else {
        m_early_preview->setText(tr("Leaving alone during the early period costs %1, split among the others.")
                                     .arg(FormatAmount(m_wallet_model, penalty)));
    }
    m_early_preview->setTextFormat(Qt::PlainText);
    m_early_warning->setVisible(blocks > 0 && penalty == 0);
    m_early_warning->setText(tr("With a zero penalty the early period has no effect: anyone can dissolve the "
                                "masternode alone at any time and pay nothing."));
}

void SharedMnCreateDialog::refreshContributionPage()
{
    const bool coordinator{m_role == Role::Coordinator};
    const auto& shares{m_session.shares()};
    m_contribution_title->setText(pageTitle(PageContribution));
    m_fee_box->setVisible(coordinator);

    m_updating = true;
    // A participant reads an invitation that names everybody; which row is
    // theirs is the one thing the invitation cannot know.
    m_who_am_i_row->setVisible(!coordinator && shares.size() > 1);
    m_who_am_i_combo->clear();
    for (size_t i = 0; i < shares.size(); ++i) {
        m_who_am_i_combo->addItem(tr("Share %1 of %2 — %3 — %4")
                                      .arg(i + 1)
                                      .arg(shares.size())
                                      .arg(FormatAmount(m_wallet_model, shares[i].amount), ShareName(m_session, i)),
                                  static_cast<int>(i));
        if (static_cast<int>(i) == m_my_share) m_who_am_i_combo->setCurrentIndex(m_who_am_i_combo->count() - 1);
    }
    m_updating = false;

    if (m_my_share >= 0 && static_cast<size_t>(m_my_share) < shares.size()) {
        m_contribution_you->setText(tr("Share %1 of %2 — %3 (you)")
                                        .arg(m_my_share + 1)
                                        .arg(shares.size())
                                        .arg(FormatAmount(m_wallet_model, shares[m_my_share].amount)));
        m_contribution_you->setTextFormat(Qt::PlainText);
    } else {
        m_contribution_you->setText(tr("Choose which participant you are."));
    }

    const MnShareSession::Contribution* mine{myContribution()};
    const bool reserved{mine != nullptr};
    if (reserved) {
        CAmount total{0};
        for (const auto& input : mine->inputs) {
            if (const auto value{resolveInputValue(input)}) total += *value;
        }
        QString text{tr("Reserved — these coins cannot be spent until the session ends or you release them.")};
        text += QLatin1Char('\n');
        const QString coins{CoinCount(mine->inputs.size())};
        text += mine->hasChange
                    ? tr("Uses %1 (%2); change %3 returns to a new address of yours.")
                          .arg(coins, FormatAmount(m_wallet_model, total),
                               FormatAmount(m_wallet_model, mine->changeAmount))
                    : tr("Uses %1 (%2), with no change left over.").arg(coins, FormatAmount(m_wallet_model, total));
        m_coins_label->setText(text);
    } else {
        const CAmount target{contributionTarget()};
        CoinSelection selection;
        QString error;
        if (target <= 0) {
            m_coins_label->setText(tr("Choose which participant you are."));
        } else if (selectCoins(target, selection, error)) {
            const QString coins{CoinCount(selection.inputs.size())};
            m_coins_label->setText(
                selection.change > 0
                    ? tr("Uses %1 (%2); change %3 returns to a new address of yours.")
                          .arg(coins, FormatAmount(m_wallet_model, selection.total),
                               FormatAmount(m_wallet_model, selection.change))
                    : tr("Uses %1 (%2), with no change left over.")
                          .arg(coins, FormatAmount(m_wallet_model, selection.total)));
        } else {
            m_coins_label->setText(error);
        }
    }

    const bool editable{m_session.stage() == MnShareSession::Stage::Draft};
    m_reserve_button->setVisible(!reserved);
    m_reserve_button->setEnabled(editable && m_my_share >= 0 && m_wallet_model != nullptr);
    m_release_button->setVisible(reserved);
    m_release_button->setEnabled(editable);
    for (QValidatedLineEdit* const edit : {m_owner_edit, m_refund_edit, m_reward_edit}) {
        edit->setReadOnly(reserved || !editable);
    }
}

void SharedMnCreateDialog::refreshInvitePage()
{
    m_lock_confirm_card->setVisible(m_lock_confirming);
    m_copy_invitation_button->setVisible(!m_lock_confirming);
    m_save_invitation_button->setVisible(!m_lock_confirming);
    m_invite_edit_warning->setVisible(!m_lock_confirming && m_replies_absorbed_revision >= 0 &&
                                      m_session.revision() > m_replies_absorbed_revision && !allDetailsCollected());
    if (m_lock_confirming) refreshTermSheets();
}

void SharedMnCreateDialog::refreshApprovalsPage()
{
    const bool coordinator{m_role == Role::Coordinator};
    m_approvals_title->setText(pageTitle(PageApprovals));
    m_approvals_purpose->setText(
        coordinator ? tr("Everyone reviews these exact terms and approves once. Send the same message to all.")
                    : tr("Read every line. Approving signs these exact terms with your owner key. Nothing is "
                         "broadcast yet."));
    m_copy_terms_button->setVisible(coordinator);
    m_unlock_button->setVisible(coordinator && m_session.stage() != MnShareSession::Stage::Combined &&
                                m_session.stage() != MnShareSession::Stage::FundingSigned &&
                                m_session.stage() != MnShareSession::Stage::Broadcast);
    m_prepare_warning_label->setVisible(!m_prepare_warning.isEmpty());
    m_prepare_warning_label->setText(m_prepare_warning);
    refreshTermSheets();
}

void SharedMnCreateDialog::refreshSignaturesPage()
{
    const bool coordinator{m_role == Role::Coordinator};
    m_signatures_title->setText(pageTitle(PageSignatures));
    m_signatures_purpose->setText(
        coordinator ? tr("All approvals are embedded. Every participant now signs their own coins. Replies can come "
                         "in any order.")
                    : tr("Signs the coins you reserved. Your signature only spends them into this registration."));
    m_copy_signing_button->setVisible(coordinator);

    const auto [signed_inputs, total_inputs]{fundingSignatureCount(m_my_share)};
    if (coordinator) {
        int done{0};
        int total{0};
        for (size_t i = 0; i < m_session.shares().size(); ++i) {
            const auto [share_signed, share_total]{fundingSignatureCount(static_cast<int>(i))};
            done += share_signed;
            total += share_total;
        }
        m_signatures_summary->setText(tr("%1 of %2 funding inputs are signed.").arg(done).arg(total));
    } else if (total_inputs > 0) {
        m_signatures_summary->setText(tr("Signs your %1 share. Nothing is broadcast by you.")
                                          .arg(FormatAmount(m_wallet_model,
                                                            m_my_share >= 0 &&
                                                                    static_cast<size_t>(m_my_share) <
                                                                        m_session.shares().size()
                                                                ? m_session.shares()[m_my_share].amount
                                                                : 0)));
    } else {
        m_signatures_summary->setText(QString());
    }
}

void SharedMnCreateDialog::refreshWaitingPages()
{
    const QString coordinator{m_session.coordinatorLabel().isEmpty() ? tr("the coordinator")
                                                                     : m_session.coordinatorLabel()};
    for (auto& [page, widgets] : m_waiting_pages) {
        widgets.title->setText(pageTitle(page));
        widgets.sent->setText(m_sent_what.isEmpty()
                                  ? QString()
                                  : tr("You sent: %1 · Code %2 · %3").arg(m_sent_what, m_sent_code, m_sent_time));
        QString expected;
        switch (page) {
        case PageWaitTerms:
            expected = tr("the locked terms");
            break;
        case PageWaitSigning:
            expected = tr("the signing request");
            break;
        default:
            expected = tr("the final record");
            break;
        }
        QString next{tr("Next you will receive %1. Paste it here when it arrives.").arg(expected)};
        if (page == PageWaitBroadcast) {
            next = tr("%1 broadcasts the registration. When the masternode appears in your list, create your standby "
                      "dissolution.")
                       .arg(coordinator);
        }
        widgets.next->setText(next);
        widgets.next->setTextFormat(Qt::PlainText);
        widgets.release->setVisible(hasLockedContributedCoins());
    }
}

void SharedMnCreateDialog::refreshCompletePage()
{
    m_complete_release_button->setVisible(hasLockedContributedCoins());
    if (m_session.stage() != MnShareSession::Stage::Broadcast) return;
    CMutableTransaction tx;
    if (DecodeHexTx(tx, m_session.protxHex().toStdString())) {
        m_complete_hash->setText(QString::fromStdString(CTransaction(tx).GetHash().ToString()));
    }
    const QString owner{m_my_share >= 0 && static_cast<size_t>(m_my_share) < m_session.shares().size()
                            ? m_session.shares()[m_my_share].ownerAddress
                            : QString()};
    m_keep_owner_hint->setText(tr("In this wallet (%1). Back up the wallet — it approves changes and dissolutions.")
                                   .arg(owner));
    m_keep_operator_hint->setText(
        tr("Held by %1. Needed on the server as masternodeblsprivkey.")
            .arg(m_session.operatorSecretHolder().isEmpty() ? tr("whoever runs the node")
                                                            : m_session.operatorSecretHolder()));

    QStringList steps;
    steps << tr("Configure the masternode server with the operator secret key and restart dashd there.");
    steps << (m_session.terms().coreP2PAddrs.isEmpty()
                  ? tr("Send a service update once the node's addresses are ready; it stays inactive until then.")
                  : tr("Make sure every registered service address and port is reachable from the internet."));
    steps << tr("Each participant should create their own standby dissolution after the first confirmation.");
    QString html{QStringLiteral("<ol>")};
    for (const QString& step : steps) {
        html += QStringLiteral("<li>%1</li>").arg(step.toHtmlEscaped());
    }
    html += QStringLiteral("</ol>");
    m_next_steps->setText(html);
}

void SharedMnCreateDialog::refreshBoards()
{
    std::vector<SharedMnStatusBoard::Row> rows;
    const auto& shares{m_session.shares()};
    rows.reserve(shares.size());
    for (size_t i = 0; i < shares.size(); ++i) {
        rows.push_back({ShareName(m_session, static_cast<int>(i)), shares[i].amount, QString()});
    }
    int replied{0};
    int approved{0};
    int fully_signed{0};
    for (const auto& [board_page, board] : m_boards) {
        board->setShares(rows);
        board->setYouRow(m_my_share);
        replied = 0;
        approved = 0;
        fully_signed = 0;
        for (int i = 0; i < static_cast<int>(shares.size()); ++i) {
            board->setCell(i, 0, hasDetails(i) ? SharedMnStatusBoard::State::Done
                                               : SharedMnStatusBoard::State::Pending);
            board->setCell(i, 1, hasFunding(i) ? SharedMnStatusBoard::State::Done
                                               : SharedMnStatusBoard::State::Pending);
            if (hasDetails(i) && hasFunding(i)) ++replied;
            const bool has_signature{!m_session.signatureFor(i).isEmpty()};
            if (has_signature) ++approved;
            board->setCell(i, 2, has_signature ? SharedMnStatusBoard::State::Done
                                               : SharedMnStatusBoard::State::Pending);
            const auto [done, total]{fundingSignatureCount(i)};
            const bool signed_off{total > 0 && done == total};
            if (signed_off) ++fully_signed;
            board->setCell(i, 3,
                           signed_off ? SharedMnStatusBoard::State::Done : SharedMnStatusBoard::State::Pending,
                           total > 0 ? tr("%1 of %2 inputs").arg(done).arg(total) : QString());
        }
        // Each board counts the round its own page is about: approvals are two
        // rounds away from the invitation, and signatures one round past them
        const int count{static_cast<int>(shares.size())};
        switch (board_page) {
        case PageInvite:
            board->setSummary(tr("%1 of %2 replied").arg(replied).arg(count));
            break;
        case PageSignatures:
        case PageWaitBroadcast:
            board->setSummary(tr("%1 of %2 signed").arg(fully_signed).arg(count));
            break;
        default:
            board->setSummary(tr("%1 of %2 approved").arg(approved).arg(count));
            break;
        }
    }
}

void SharedMnCreateDialog::refreshTermSheets()
{
    const QString html{SharedMnTermSheetHtml(m_session, m_my_share, SharedMnDisplayUnit(m_wallet_model))};
    m_approvals_terms->setText(html);
    m_lock_terms_label->setText(html);
    bool fatal{false};
    m_lock_funding_label->setText(fundingCheck(fatal));
}

void SharedMnCreateDialog::syncSharesFromTable()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    auto& shares{m_session.shares()};
    bool changed{false};
    for (int row = 0; row < m_share_table->rowCount() && static_cast<size_t>(row) < shares.size(); ++row) {
        if (auto* name = qobject_cast<QLineEdit*>(m_share_table->cellWidget(row, SHARE_COL_NAME))) {
            const QString text{name->text().trimmed()};
            if (shares[row].label != text) {
                shares[row].label = text;
                changed = true;
            }
        }
        if (auto* amount = qobject_cast<BitcoinAmountField*>(m_share_table->cellWidget(row, SHARE_COL_AMOUNT))) {
            if (shares[row].amount != amount->value()) {
                shares[row].amount = amount->value();
                changed = true;
            }
        }
    }
    if (const int checked{m_me_group->checkedId()}; checked >= 0 && checked != m_my_share) {
        m_my_share = checked;
    }
    // Name the coordinator as soon as the roster does, so participants see
    // "Waiting for Alice" from the first round and a reopened session can tell
    // whose share coordinates it
    if (m_role == Role::Coordinator && m_my_share >= 0 && static_cast<size_t>(m_my_share) < shares.size() &&
        m_session.coordinatorLabel() != shares[m_my_share].label) {
        m_session.setCoordinatorLabel(shares[m_my_share].label);
    }
    if (changed) {
        m_session.noteDraftChange();
        m_dirty = true;
    }
}

void SharedMnCreateDialog::syncTermsToSession()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    auto& terms{m_session.terms()};
    const MnShareSession::Terms before{terms};
    terms.coreP2PAddrs = MasternodeWidgetUtil::tokenizeEndpointList(m_service_edit->text()).join(QLatin1Char(','));
    if (!m_operator_key_from_import) terms.operatorPubKey = m_operator_widget->publicKeyHex();
    terms.votingAddress = m_voting_edit->text().trimmed();
    terms.operatorReward = static_cast<int>(qRound(m_operator_reward_spin->value() * 100.0));
    terms.earlyPeriodBlocks = static_cast<uint32_t>(m_early_period_spin->value());
    terms.earlyPenalty = m_early_penalty_field->value();
    const QString holder{m_node_run_by_edit->text().trimmed()};
    const bool changed{before.coreP2PAddrs != terms.coreP2PAddrs || before.operatorPubKey != terms.operatorPubKey ||
                       before.votingAddress != terms.votingAddress ||
                       before.operatorReward != terms.operatorReward ||
                       before.earlyPeriodBlocks != terms.earlyPeriodBlocks ||
                       before.earlyPenalty != terms.earlyPenalty ||
                       m_session.operatorSecretHolder() != holder};
    m_session.setOperatorSecretHolder(holder);
    if (changed) {
        m_session.noteDraftChange();
        m_dirty = true;
    }
}

void SharedMnCreateDialog::pushSessionToWidgets()
{
    m_updating = true;
    const auto& terms{m_session.terms()};
    m_service_edit->setText(terms.coreP2PAddrs);
    m_voting_edit->setText(terms.votingAddress);
    m_operator_reward_spin->setValue(terms.operatorReward / 100.0);
    m_early_period_spin->setValue(static_cast<int>(terms.earlyPeriodBlocks));
    m_early_penalty_field->setValue(terms.earlyPenalty);
    m_node_run_by_edit->setText(m_session.operatorSecretHolder());

    // An imported envelope carries the key the group already agreed on; the
    // generate widget must never silently replace it. That holds for a
    // coordinator reopening their own saved session too: the key went out with
    // the invitation and every collected approval covers it.
    m_operator_key_from_import = m_session_imported && !terms.operatorPubKey.isEmpty();
    m_operator_widget->setVisible(!m_operator_key_from_import);
    m_operator_imported_label->setVisible(m_operator_key_from_import);
    m_operator_imported_label->setText(MasternodeWidgetUtil::chunked(terms.operatorPubKey));

    const bool editable{m_session.stage() == MnShareSession::Stage::Draft};
    for (QWidget* const widget : {static_cast<QWidget*>(m_service_edit), static_cast<QWidget*>(m_voting_edit),
                                  static_cast<QWidget*>(m_node_run_by_edit),
                                  static_cast<QWidget*>(m_operator_reward_spin),
                                  static_cast<QWidget*>(m_early_period_spin),
                                  static_cast<QWidget*>(m_early_penalty_field)}) {
        widget->setEnabled(editable && m_role == Role::Coordinator);
    }

    if (m_my_share >= 0 && static_cast<size_t>(m_my_share) < m_session.shares().size()) {
        const auto& share{m_session.shares()[m_my_share]};
        if (!share.ownerAddress.isEmpty()) m_owner_edit->setText(share.ownerAddress);
        if (!share.refundAddress.isEmpty()) m_refund_edit->setText(share.refundAddress);
        if (!share.rewardAddress.isEmpty()) m_reward_edit->setText(share.rewardAddress);
    }
    m_updating = false;
}

void SharedMnCreateDialog::startSession()
{
    if (!m_v24_active) {
        showError(tr("Shared masternodes need the v24 upgrade, which is not active on this network yet."));
        return;
    }
    if (m_wallet_model == nullptr) {
        showError(tr("No wallet is available."));
        return;
    }
    if (!canSign()) {
        showError(tr("This wallet is watch-only and cannot sign."));
        return;
    }
    unlockAllContributedCoins();
    m_session = MnShareSession();
    m_role = Role::Coordinator;
    m_my_share = 0;
    m_prepare_warning.clear();
    m_dead_reason.clear();
    m_foreign_input.clear();
    m_replies_absorbed_revision = -1;
    m_session_imported = false;
    m_operator_key_from_import = false;

    // Two named rows splitting the collateral evenly are a working starting
    // point; every one of them is still editable.
    const CAmount collateral{GetMnType(MnType::Regular).collat_amount};
    auto& shares{m_session.shares()};
    shares.assign(CProRegTx::MIN_SHARES, MnShareSession::Share{});
    for (size_t i = 0; i < shares.size(); ++i) {
        // Named rather than blank: two empty name cells look optional until
        // Next refuses to move on
        shares[i].label = tr("Participant %1").arg(i + 1);
    }
    shares[0].amount = collateral / 2;
    shares[1].amount = collateral - shares[0].amount;
    m_session.setPrepareWallet(m_wallet_model->getWalletName());
    m_dirty = true;

    rebuildOrder();
    refreshAll();
    goToPage(PageParticipants);
}

void SharedMnCreateDialog::beginLockConfirmation()
{
    m_lock_confirming = true;
    refreshInvitePage();
    updateButtons();
}

void SharedMnCreateDialog::lockTerms()
{
    if (m_busy || m_session.stage() != MnShareSession::Stage::Draft) return;
    syncTermsToSession();
    if (const QStringList problems{m_session.validateShares()}; !problems.isEmpty()) {
        showError(problems.join(QLatin1Char('\n')));
        return;
    }
    bool fatal{false};
    const QString funding{fundingCheck(fatal)};
    if (fatal) {
        showError(funding);
        return;
    }

    const auto& terms{m_session.terms()};
    UniValue params(UniValue::VOBJ);
    params.pushKV("fundingTx", m_session.fundingTxHex().toStdString());
    UniValue shares(UniValue::VARR);
    for (const auto& share : m_session.shares()) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("amount", share.amount);
        entry.pushKV("refundAddress", share.refundAddress.toStdString());
        if (!share.rewardAddress.isEmpty()) entry.pushKV("rewardAddress", share.rewardAddress.toStdString());
        entry.pushKV("ownerAddress", share.ownerAddress.toStdString());
        shares.push_back(entry);
    }
    params.pushKV("shares", shares);
    // The RPC declares coreP2PAddrs as a string but also accepts an array;
    // a single string is one endpoint, so several endpoints go as an array
    const QStringList endpoints{MasternodeWidgetUtil::tokenizeEndpointList(terms.coreP2PAddrs)};
    if (endpoints.size() <= 1) {
        params.pushKV("coreP2PAddrs", endpoints.value(0).toStdString());
    } else {
        UniValue services(UniValue::VARR);
        for (const QString& service : endpoints) {
            services.push_back(service.toStdString());
        }
        params.pushKV("coreP2PAddrs", services);
    }
    params.pushKV("operatorPubKey", terms.operatorPubKey.toStdString());
    params.pushKV("votingAddress", terms.votingAddress.toStdString());
    params.pushKV("operatorReward", QStringLiteral("%1.%2")
                                        .arg(terms.operatorReward / 100)
                                        .arg(terms.operatorReward % 100, 2, 10, QLatin1Char('0'))
                                        .toStdString());
    params.pushKV("earlyPeriodBlocks", static_cast<int64_t>(terms.earlyPeriodBlocks));
    params.pushKV("earlyPenalty", terms.earlyPenalty);

    // shared_register_prepare only assembles the transaction; no keys involved
    setBusy(true, tr("Preparing…"));
    ProTxResult result;
    const bool ran{runRpc(shared_mn_rpc::REGISTER_PREPARE, params, /*needs_unlock=*/false, result)};
    setBusy(false);
    if (!ran) return;
    if (!result.ok) {
        showError(tr("Preparing the registration failed: %1").arg(result.message));
        return;
    }
    const UniValue& tx{result.value.find_value("tx")};
    const UniValue& collateral_index{result.value.find_value("collateralIndex")};
    const UniValue& consent_hash{result.value.find_value("consentHash")};
    if (!result.value.isObject() || !tx.isStr() || !collateral_index.isNum() || !consent_hash.isStr()) {
        showError(tr("Unexpected reply from the node."));
        return;
    }
    QString error;
    if (!m_session.freeze(QString::fromStdString(tx.get_str()), QString::fromStdString(consent_hash.get_str()),
                          collateral_index.getInt<int>(), error)) {
        showError(tr("Preparing the registration failed: %1").arg(error));
        return;
    }
    if (m_wallet_model != nullptr) m_session.setPrepareWallet(m_wallet_model->getWalletName());
    // The advisory only comes back from the prepare call, so it is shown on the
    // approvals page while the terms can still be unlocked
    const UniValue& warning{result.value.find_value("warning")};
    m_prepare_warning = warning.isStr() ? QString::fromStdString(warning.get_str()) : QString{};
    m_lock_confirming = false;
    m_dirty = true;

    // Approving the terms one just locked needs no separate decision. The page
    // switch clears the error line, so the reason a failed approval gives is
    // shown after it, not before.
    const bool approved{signOwnConsent(error)};
    refreshAll();
    goToPage(PageApprovals);
    if (!approved) showError(error);
}

bool SharedMnCreateDialog::signOwnConsent(QString& error)
{
    error.clear();
    if (m_session.stage() != MnShareSession::Stage::Frozen && m_session.stage() != MnShareSession::Stage::Signing) {
        error = tr("The terms are not locked yet.");
        return false;
    }
    if (!canSign()) {
        error = tr("This wallet is watch-only and cannot sign.");
        return false;
    }
    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", m_session.protxHex().toStdString());
    setBusy(true, tr("Approving…"));
    ProTxResult result;
    const bool ran{runRpc(shared_mn_rpc::SIGN, params, /*needs_unlock=*/true, result)};
    setBusy(false);
    if (!ran) {
        error = tr("Approving failed: %1").arg(tr("the wallet stayed locked."));
        return false;
    }
    if (!result.ok) {
        error = tr("Approving failed: %1").arg(result.message);
        return false;
    }
    const UniValue& signatures{result.value.isObject() ? result.value.find_value("signatures") : NullUniValue};
    if (!signatures.isArray()) {
        error = tr("Unexpected reply from the node.");
        return false;
    }
    QStringList problems;
    for (const auto& entry : signatures.getValues()) {
        try {
            const int share_index{entry.find_value("shareIndex").getInt<int>()};
            const QString signature{QString::fromStdString(entry.find_value("signature").get_str())};
            if (QString add_error; !m_session.addSignature(share_index, signature, add_error)) {
                problems << add_error;
            }
        } catch (const std::exception& e) {
            problems << QString::fromUtf8(e.what());
        }
    }
    m_dirty = true;
    if (!problems.isEmpty()) {
        error = problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

void SharedMnCreateDialog::approveAndCopy()
{
    QString error;
    if (!signOwnConsent(error)) {
        showError(error);
        refreshAll();
        return;
    }
    refreshAll();
    copySession(tr("Approval"));
    goToPage(PageWaitSigning);
}

bool SharedMnCreateDialog::combineApprovals(QString& error)
{
    error.clear();
    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", m_session.protxHex().toStdString());
    UniValue signatures(UniValue::VARR);
    for (const auto& sig : m_session.signatures()) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("shareIndex", sig.shareIndex);
        entry.pushKV("signature", sig.signatureB64.toStdString());
        signatures.push_back(entry);
    }
    params.pushKV("signatures", signatures);
    params.pushKV("submit", false);

    setBusy(true, tr("Combining approvals…"));
    ProTxResult result;
    const bool ran{runRpc(shared_mn_rpc::COMBINE, params, /*needs_unlock=*/false, result)};
    setBusy(false);
    if (!ran) {
        error = tr("Combining approvals failed: %1").arg(tr("the request could not be started."));
        return false;
    }
    if (!result.ok) {
        error = tr("Combining approvals failed: %1").arg(result.message);
        return false;
    }
    if (!result.value.isStr()) {
        error = tr("Unexpected reply from the node.");
        return false;
    }
    if (QString set_error; !m_session.setCombinedTx(QString::fromStdString(result.value.get_str()), set_error)) {
        error = tr("Combining approvals failed: %1").arg(set_error);
        return false;
    }
    m_dirty = true;
    return true;
}

QStringList SharedMnCreateDialog::foreignWalletInputs() const
{
    QStringList ret;
    if (m_wallet_model == nullptr) return ret;
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, m_session.protxHex().toStdString())) return ret;
    const MnShareSession::Contribution* const mine{myContribution()};
    for (const CTxIn& txin : tx.vin) {
        if (mine != nullptr && ContributesOutpoint(mine->inputs, txin.prevout)) continue;
        if ((m_wallet_model->wallet().txinIsMine(txin) & wallet::ISMINE_SPENDABLE) == 0) continue;
        ret << OutpointKey(txin.prevout);
    }
    return ret;
}

bool SharedMnCreateDialog::signedOnlyOwnInputs(const QString& before_hex, const QString& after_hex,
                                               const MnShareSession::Contribution* mine, QString& outpoint)
{
    outpoint.clear();
    CMutableTransaction before;
    CMutableTransaction after;
    if (!DecodeHexTx(before, before_hex.toStdString()) || !DecodeHexTx(after, after_hex.toStdString())) return false;
    if (before.vin.size() != after.vin.size()) return false;
    for (size_t i = 0; i < after.vin.size(); ++i) {
        if (after.vin[i] == before.vin[i]) continue;
        if (mine != nullptr && ContributesOutpoint(mine->inputs, after.vin[i].prevout) &&
            ContributesOutpoint(mine->inputs, before.vin[i].prevout)) {
            continue;
        }
        outpoint = OutpointKey(after.vin[i].prevout);
        return false;
    }
    return true;
}

QString SharedMnCreateDialog::foreignInputError(const QString& outpoint) const
{
    return tr("This transaction also spends %1, a coin of this wallet that is not part of your contribution. Do not "
              "sign; ask %2 for a fresh session.")
        .arg(outpoint, recipientName());
}

void SharedMnCreateDialog::checkForeignInputs()
{
    const QStringList foreign{foreignWalletInputs()};
    m_foreign_input = foreign.isEmpty() ? QString() : foreign.front();
}

bool SharedMnCreateDialog::signOwnFundingInputs(bool& complete, QString& error)
{
    complete = false;
    error.clear();
    if (!canSign()) {
        error = tr("This wallet is watch-only and cannot sign.");
        return false;
    }
    // "signrawtransactionwithwallet" signs every input this wallet can sign,
    // so a coin of ours recorded under somebody else's contribution would be
    // signed away with our own. Refuse before the wallet is even unlocked.
    checkForeignInputs();
    if (!m_foreign_input.isEmpty()) {
        error = foreignInputError(m_foreign_input);
        return false;
    }
    const QString before{m_session.protxHex()};
    UniValue params(UniValue::VOBJ);
    params.pushKV("hexstring", before.toStdString());
    setBusy(true, tr("Signing your contribution…"));
    ProTxResult result;
    const bool ran{runRpc(QStringLiteral("signrawtransactionwithwallet"), params, /*needs_unlock=*/true, result)};
    setBusy(false);
    if (!ran) {
        error = tr("Signing failed: %1").arg(tr("the wallet stayed locked."));
        return false;
    }
    if (!result.ok) {
        error = tr("Signing failed: %1").arg(result.message);
        return false;
    }
    const UniValue& hex{result.value.find_value("hex")};
    const UniValue& done{result.value.find_value("complete")};
    if (!result.value.isObject() || !hex.isStr() || !done.isBool()) {
        error = tr("Unexpected reply from the node.");
        return false;
    }
    const QString signed_hex{QString::fromStdString(hex.get_str())};
    complete = done.get_bool();
    if (QString outpoint; !signedOnlyOwnInputs(before, signed_hex, myContribution(), outpoint)) {
        if (!outpoint.isEmpty()) m_foreign_input = outpoint;
        error = outpoint.isEmpty()
                    ? tr("The signed transaction is not the one you reviewed. Do not use it; ask %1 for a fresh "
                         "session.")
                          .arg(recipientName())
                    : foreignInputError(outpoint);
        return false;
    }
    QString apply_error;
    const bool applied{complete ? m_session.setFundingSignedTx(signed_hex, apply_error)
                                : replaceSessionProTx(signed_hex, apply_error)};
    if (!applied) {
        error = tr("Signing failed: %1").arg(apply_error);
        return false;
    }
    m_dirty = true;
    return true;
}

void SharedMnCreateDialog::runCombineAndSign()
{
    if (m_busy || m_role != Role::Coordinator || !allApproved()) return;
    QString error;
    if (m_session.stage() == MnShareSession::Stage::Frozen ||
        m_session.stage() == MnShareSession::Stage::Signing) {
        if (!combineApprovals(error)) {
            showError(error);
            refreshAll();
            return;
        }
    }
    if (m_session.stage() == MnShareSession::Stage::Combined) {
        bool complete{false};
        if (!signOwnFundingInputs(complete, error)) {
            showError(error);
            refreshAll();
            return;
        }
    }
    refreshAll();
    goToPage(PageSignatures);
}

void SharedMnCreateDialog::signAndCopy()
{
    if (m_busy) return;
    bool complete{false};
    QString error;
    if (!signOwnFundingInputs(complete, error)) {
        showError(error);
        refreshAll();
        return;
    }
    refreshAll();
    copySession(tr("Signed Contribution"));
    goToPage(PageWaitBroadcast);
}

void SharedMnCreateDialog::unlockTerms()
{
    if (m_busy) return;
    const int approvals{m_session.signedCount()};
    const QString discarded{
        SharedMnPlural(approvals, QT_TRANSLATE_NOOP("MnShareSession", "1 approval"),
                       QT_TRANSLATE_NOOP("MnShareSession", "%1 approvals"))};
    const auto choice{QMessageBox::question(this, tr("Unlock the terms?"),
                                            tr("Unlocking discards %1. Everyone must approve again after you lock.")
                                                .arg(discarded),
                                            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)};
    if (choice != QMessageBox::Yes) return;
    m_session.unfreeze();
    m_prepare_warning.clear();
    m_dirty = true;
    refreshAll();
    goToPage(PageInvite);
}

bool SharedMnCreateDialog::confirmBroadcast() const
{
    const CAmount collateral{GetMnType(MnType::Regular).collat_amount};
    bool fatal{false};
    const QString participants{
        SharedMnPlural(static_cast<qint64>(m_session.shares().size()),
                       QT_TRANSLATE_NOOP("MnShareSession", "1 participant"),
                       QT_TRANSLATE_NOOP("MnShareSession", "%1 participants"))};
    QStringList detail;
    detail << tr("Collateral: %1").arg(FormatAmount(m_wallet_model, collateral));
    detail << tr("Participants: %1").arg(participants);
    detail << fundingCheck(fatal);
    SendConfirmationDialog confirmation{
        tr("Confirm shared masternode registration"),
        tr("Broadcast this shared masternode registration?"),
        detail.join(QLatin1Char('\n')),
        tr("Session %1 · Code %2").arg(m_session.sessionCode(), m_session.fingerprint()),
        SEND_CONFIRM_DELAY,
        /*enable_send=*/true,
        /*always_show_unsigned=*/false,
        const_cast<SharedMnCreateDialog*>(this)};
    confirmation.setWindowModality(Qt::WindowModal);
    // A QMessageBox is only as wide as its longest line, which wraps a
    // thousands separator across lines; make it wide enough to read
    if (auto* const grid = qobject_cast<QGridLayout*>(confirmation.layout()); grid != nullptr) {
        grid->addItem(new QSpacerItem(CONFIRM_MINIMUM_WIDTH, 0, QSizePolicy::Minimum, QSizePolicy::Expanding),
                      grid->rowCount(), 0, 1, grid->columnCount());
    }
    return confirmation.exec() == QMessageBox::Yes;
}

void SharedMnCreateDialog::broadcastRegistration()
{
    if (m_busy || m_session.stage() != MnShareSession::Stage::FundingSigned) return;
    if (!confirmBroadcast()) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("hexstring", m_session.protxHex().toStdString());
    setBusy(true, tr("Broadcasting…"));
    ProTxResult result;
    const bool ran{runRpc(QStringLiteral("sendrawtransaction"), params, /*needs_unlock=*/false, result)};
    setBusy(false);
    if (!ran) return;
    if (!result.ok) {
        showError(tr("Broadcast failed: %1").arg(result.message));
        return;
    }
    if (QString error; !m_session.setBroadcast(error)) {
        showError(tr("Broadcast failed: %1").arg(error));
        return;
    }
    // The contributed coins are spent by the registration now
    unlockAllContributedCoins();
    m_dirty = true;
    refreshAll();
    goToPage(PageComplete);
}

QString SharedMnCreateDialog::messageName(const MnShareSession& session, bool from_coordinator) const
{
    switch (session.stage()) {
    case MnShareSession::Stage::Draft:
        return from_coordinator ? tr("Invitation") : tr("Details");
    case MnShareSession::Stage::Frozen:
    case MnShareSession::Stage::Signing:
        return from_coordinator ? tr("Locked Terms") : tr("Approval");
    case MnShareSession::Stage::Combined:
        return from_coordinator ? tr("Signing Request") : tr("Signed Contribution");
    case MnShareSession::Stage::FundingSigned:
        return tr("Signed Contribution");
    case MnShareSession::Stage::Broadcast:
        break;
    }
    return tr("Final Record");
}

QString SharedMnCreateDialog::senderName(const MnShareSession& before) const
{
    const auto& now{m_session.shares()};
    if (before.shares().size() != now.size()) return {};
    const auto signature_map{InputSignatureMap(m_session.protxHex())};
    const auto before_map{InputSignatureMap(before.protxHex())};
    for (size_t i = 0; i < now.size(); ++i) {
        if (before.shares()[i].ownerAddress.isEmpty() && !now[i].ownerAddress.isEmpty()) {
            return ShareName(m_session, static_cast<int>(i));
        }
        if (before.signatureFor(static_cast<int>(i)).isEmpty() &&
            !m_session.signatureFor(static_cast<int>(i)).isEmpty()) {
            return ShareName(m_session, static_cast<int>(i));
        }
        for (const auto& contribution : m_session.contributions()) {
            if (contribution.label != now[i].label) continue;
            for (const auto& input : contribution.inputs) {
                const QString key{OutpointKey(input.txid, input.vout)};
                const auto after{signature_map.find(key)};
                const auto prior{before_map.find(key)};
                if (after != signature_map.end() && after->second &&
                    (prior == before_map.end() || !prior->second)) {
                    return ShareName(m_session, static_cast<int>(i));
                }
            }
        }
    }
    return {};
}

QString SharedMnCreateDialog::recipientName() const
{
    if (m_role == Role::Coordinator) return tr("everyone");
    return m_session.coordinatorLabel().isEmpty() ? tr("the coordinator") : m_session.coordinatorLabel();
}

void SharedMnCreateDialog::copySession(const QString& what)
{
    GUIUtil::setClipboard(m_session.toJsonString());
    m_sent_what = what;
    m_sent_code = m_session.fingerprint();
    m_sent_time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
    showStatus(tr("Copied · Session %1 · Code %2 — send it to %3.")
                   .arg(m_session.sessionCode(), m_sent_code, recipientName()));
    refreshWaitingPages();
}

void SharedMnCreateDialog::saveSession()
{
    if (operatorSecretUnsaved()) {
        // The session file carries only the operator public key, so a file
        // saved now would describe a masternode nobody can run
        goToPage(PageSecret);
        showError(tr("Save the operator secret key first. It is shown only here, and the session file does not "
                     "carry it: without it the masternode could never be run."));
        return;
    }
    const QString suggested{QStringLiteral("shared-mn-session-%1.json").arg(m_session.sessionCode())};
    const QString filename{GUIUtil::getSaveFileName(this, tr("Save shared masternode message"), suggested,
                                                    tr("Session files (*.json)"), nullptr)};
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        showError(tr("Could not open %1 for writing.").arg(filename));
        return;
    }
    file.write(m_session.toJsonString().toUtf8());
    file.close();
    m_dirty = false;
    showStatus(tr("Saved · Session %1 · Code %2").arg(m_session.sessionCode(), m_session.fingerprint()));
}

void SharedMnCreateDialog::onSave()
{
    saveSession();
}

void SharedMnCreateDialog::onPaste()
{
    handleImportedText(QApplication::clipboard()->text());
}

void SharedMnCreateDialog::onOpenFile()
{
    const QString filename{GUIUtil::getOpenFileName(this, tr("Open shared masternode message"), QString(),
                                                    tr("Session files (*.json)"), nullptr)};
    if (filename.isEmpty()) return;
    if (QFileInfo(filename).size() > MAX_ENVELOPE_FILE_BYTES) {
        showError(tr("That message is too large to be a shared masternode message."));
        return;
    }
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        showError(tr("Could not open %1 for reading.").arg(filename));
        return;
    }
    handleImportedText(QString::fromUtf8(file.readAll()));
}

void SharedMnCreateDialog::openSharedMessage(const QString& text)
{
    handleImportedText(text);
}

void SharedMnCreateDialog::handleImportedText(const QString& text)
{
    if (text.toUtf8().size() > MAX_ENVELOPE_FILE_BYTES) {
        // Parsing megabytes of JSON, or building a widget row per share in it,
        // happens on the GUI thread; a session message is a few kilobytes
        showError(tr("That message is too large to be a shared masternode message."));
        return;
    }
    const SharedMnImport::Detected detected{SharedMnImport::Detect(text)};
    switch (detected.kind) {
    case SharedMnImport::Kind::Session:
        break;
    case SharedMnImport::Kind::Sigs:
    case SharedMnImport::Kind::StandbyHex:
        // Not this dialog's message: only the masternode list can resolve the
        // proTxHash it carries to the masternode it belongs to.
        if (!OpenSharedMaintenanceDialog(this, text)) {
            showError(tr("This message is about an existing masternode. Open it from the Masternodes list."));
        }
        return;
    case SharedMnImport::Kind::Unknown:
        showError(detected.error);
        return;
    }

    MnShareSession imported;
    QString error;
    if (!imported.fromJson(text.toStdString(), error)) {
        showError(error);
        return;
    }
    const QString warning{imported.importWarning()};
    if (m_role == Role::Undecided) {
        replaceSession(imported);
    } else {
        absorbSession(imported);
    }
    // Both of those land on another page, which clears the message lines
    if (!warning.isEmpty()) showStatus(warning);
}

void SharedMnCreateDialog::replaceSession(const MnShareSession& imported)
{
    unlockAllContributedCoins();
    const bool ours{coordinatedHere(imported)};
    m_session = imported;
    if (m_role == Role::Undecided) m_role = ours ? Role::Coordinator : Role::Participant;
    m_session_imported = true;
    m_my_share = -1;
    m_prepare_warning.clear();
    m_dead_reason.clear();
    m_foreign_input.clear();
    m_replies_absorbed_revision = -1;

    // An adopted envelope's signatures were stored as-is by fromJson; drop any
    // that do not verify so they are never counted.
    QStringList bad;
    for (const auto& check : m_session.verifyAllSignatures()) {
        if (!check.valid) bad << check.error;
    }
    if (!bad.isEmpty()) {
        UniValue json{m_session.toJson()};
        UniValue sigs(UniValue::VARR);
        for (const auto& sig : m_session.signatures()) {
            QString sig_error;
            if (!m_session.verifySignature(sig.shareIndex, sig.signatureB64, sig_error)) continue;
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("shareIndex", sig.shareIndex);
            entry.pushKV("signature", sig.signatureB64.toStdString());
            sigs.push_back(entry);
        }
        json.pushKV("sigs", sigs);
        QString error;
        m_session.fromJson(json, error); // leaves the session untouched on failure
        ShowPlainMessage(this, QMessageBox::Warning, tr("Signatures not counted"), bad.join(QLatin1Char('\n')));
    }

    inferMyShare();
    checkSessionLiveness();
    checkForeignInputs();
    if (m_dead_reason.isEmpty()) refreshContributedCoinLocks();
    rebuildOrder();
    refreshAll();
    const QString received{tr("Received %1 · Code %2")
                               .arg(messageName(m_session, /*from_coordinator=*/true), m_session.fingerprint())};
    for (const auto& [board_page, board] : m_boards) {
        board->setLastReceived(received);
    }

    switch (m_session.stage()) {
    case MnShareSession::Stage::Draft:
        // The coordinator's own drafting pages stay reachable with Back; the
        // invite page is where the round a saved draft is in continues
        goToPage(m_role == Role::Coordinator ? PageInvite : PageContribution);
        break;
    case MnShareSession::Stage::Frozen:
    case MnShareSession::Stage::Signing:
        goToPage(PageApprovals);
        break;
    case MnShareSession::Stage::Combined:
        goToPage(PageSignatures);
        break;
    case MnShareSession::Stage::FundingSigned:
        // Only the coordinator broadcasts, and its page order has no waiting
        // page: the Signatures page is where "Broadcast Registration" lives
        goToPage(m_role == Role::Coordinator ? PageSignatures : PageWaitBroadcast);
        break;
    case MnShareSession::Stage::Broadcast:
        goToPage(PageComplete);
        break;
    }
}

void SharedMnCreateDialog::absorbSession(const MnShareSession& imported)
{
    const MnShareSession before{m_session};
    QString error;
    bool absorbed{false};

    if (m_session.stage() == MnShareSession::Stage::Draft &&
        imported.stage() == MnShareSession::Stage::Draft) {
        // A reply to this exact invitation: only ever fills empty rows in, so
        // replies can be pasted in any order.
        absorbed = m_session.absorbDraftReply(imported, error) == MnShareSession::MergeResult::Merged;
        if (!absorbed) {
            showError(error);
            return;
        }
        m_replies_absorbed_revision = m_session.revision();
    } else if (m_session.stage() == MnShareSession::Stage::Draft) {
        absorbed = m_session.adoptLockedTerms(imported, error);
        if (!absorbed) {
            showError(error);
            return;
        }
    } else {
        switch (m_session.mergeEnvelope(imported, error)) {
        case MnShareSession::MergeResult::Merged:
            absorbed = true;
            break;
        case MnShareSession::MergeResult::OtherOlder:
            showError(tr("This reply is older than your current terms (Code %1; yours is %2). Ask the sender to "
                         "paste your latest message and reply again.")
                          .arg(imported.fingerprint(), m_session.fingerprint()));
            return;
        case MnShareSession::MergeResult::OtherNewer:
            replaceSession(imported);
            return;
        case MnShareSession::MergeResult::Conflict:
            showError(error);
            return;
        }
    }
    if (!absorbed) return;

    m_dirty = true;
    inferMyShare();
    checkSessionLiveness();
    checkForeignInputs();
    if (m_dead_reason.isEmpty()) refreshContributedCoinLocks();
    rebuildOrder();

    const QString sender{senderName(before)};
    const bool from_coordinator{m_role == Role::Participant};
    const QString received{sender.isEmpty()
                               ? tr("Received %1 · Code %2")
                                     .arg(messageName(m_session, from_coordinator), m_session.fingerprint())
                               : tr("Received %1's %2 · Code %3")
                                     .arg(sender, messageName(m_session, from_coordinator),
                                          m_session.fingerprint())};
    refreshAll();
    for (const auto& [board_page, board] : m_boards) {
        board->setLastReceived(received);
    }

    // A participant follows the coordinator's stage; the coordinator only
    // moves on once every reply of the round is in.
    if (m_role == Role::Participant) {
        switch (m_session.stage()) {
        case MnShareSession::Stage::Frozen:
        case MnShareSession::Stage::Signing:
            goToPage(PageApprovals);
            break;
        case MnShareSession::Stage::Combined:
            goToPage(PageSignatures);
            break;
        case MnShareSession::Stage::Broadcast:
            goToPage(PageComplete);
            break;
        default:
            break;
        }
        return;
    }
    if (m_session.stage() == MnShareSession::Stage::FundingSigned && currentPage() == PageSignatures) {
        updateButtons();
    } else if (allApproved() && (m_session.stage() == MnShareSession::Stage::Frozen ||
                                 m_session.stage() == MnShareSession::Stage::Signing)) {
        // Combining and signing this wallet's own coins need no decision
        runCombineAndSign();
    }
}

QString SharedMnCreateDialog::freshAddress(QString& error) const
{
    error.clear();
    if (m_wallet_model == nullptr) {
        error = tr("No wallet is available.");
        return {};
    }
    auto dest{m_wallet_model->wallet().getNewDestination(/*label=*/"")};
    if (!dest) {
        error = tr("Could not generate a new address: %1")
                    .arg(QString::fromStdString(util::ErrorString(dest).translated));
        return {};
    }
    return QString::fromStdString(EncodeDestination(*dest));
}

CAmount SharedMnCreateDialog::contributionTarget() const
{
    if (m_my_share < 0 || static_cast<size_t>(m_my_share) >= m_session.shares().size()) return 0;
    const CAmount share{m_session.shares()[m_my_share].amount};
    return m_role == Role::Coordinator ? share + m_fee_field->value() : share;
}

bool SharedMnCreateDialog::selectCoins(CAmount target, CoinSelection& selection, QString& error) const
{
    selection = CoinSelection{};
    error.clear();
    if (m_wallet_model == nullptr) {
        error = tr("No wallet is available.");
        return false;
    }
    if (target <= 0) {
        error = tr("Select the participant that is you.");
        return false;
    }

    std::vector<std::pair<CAmount, COutPoint>> candidates;
    for (const auto& [dest, coins] : m_wallet_model->wallet().listCoins()) {
        for (const auto& [outpoint, txout] : coins) {
            if (txout.is_spent || txout.depth_in_main_chain < 1) continue;
            if (m_wallet_model->wallet().isLockedCoin(outpoint)) continue;
            if (!m_wallet_model->wallet().isSpendable(txout.txout.scriptPubKey)) continue;
            candidates.emplace_back(txout.txout.nValue, outpoint);
        }
    }
    // Largest first keeps the input count, and with it everyone's signing work
    // and the transaction fee, as small as possible.
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });

    // A change output below the dust threshold cannot be relayed, so keep
    // adding coins until the change clears it; if nothing does, drop the
    // change and let the remainder go to the fee.
    const auto is_dust = [this](CAmount value) {
        const CTxOut probe{value, GetScriptForDestination(PKHash{CKeyID{}})};
        return IsDust(probe, m_node.getDustRelayFee());
    };
    size_t used{0};
    for (; used < candidates.size(); ++used) {
        if (selection.total >= target && (selection.total == target || !is_dust(selection.total - target))) break;
        selection.total += candidates[used].first;
        MnShareSession::Input input;
        input.txid = QString::fromStdString(candidates[used].second.hash.ToString());
        input.vout = candidates[used].second.n;
        selection.inputs.push_back(input);
    }
    if (selection.total < target) {
        const CAmount share{m_my_share >= 0 && static_cast<size_t>(m_my_share) < m_session.shares().size()
                                ? m_session.shares()[m_my_share].amount
                                : target};
        error = tr("This wallet needs at least %1 spendable to fund your %2 share.")
                    .arg(FormatAmount(m_wallet_model, target), FormatAmount(m_wallet_model, share));
        selection = CoinSelection{};
        return false;
    }
    const CAmount change{selection.total - target};
    selection.change = (change > 0 && !is_dust(change)) ? change : 0;
    return true;
}

void SharedMnCreateDialog::reserveCoins()
{
    if (m_wallet_model == nullptr || m_session.stage() != MnShareSession::Stage::Draft) return;
    if (m_my_share < 0 || static_cast<size_t>(m_my_share) >= m_session.shares().size()) {
        showError(tr("Select the participant that is you."));
        return;
    }
    // The reservation fixes these addresses in the share row, so they have to
    // be right before any coin is locked
    const QString owner{m_owner_edit->text().trimmed()};
    const QString refund{m_refund_edit->text().trimmed()};
    if (!MasternodeWidgetUtil::isP2PKHAddress(owner)) {
        showError(tr("Enter a valid owner address (P2PKH)."));
        return;
    }
    if (!MasternodeWidgetUtil::isP2PKHorP2SHAddress(refund)) {
        showError(tr("Enter a valid refund address (P2PKH or P2SH)."));
        return;
    }

    QString error;
    auto& share{m_session.shares()[m_my_share]};
    share.ownerAddress = owner;
    share.refundAddress = refund;
    share.rewardAddress = m_reward_edit->text().trimmed();
    if (share.label.isEmpty()) {
        showError(tr("Enter a name for participant %1.").arg(m_my_share + 1));
        return;
    }

    CoinSelection selection;
    if (!selectCoins(contributionTarget(), selection, error)) {
        showError(error);
        return;
    }
    MnShareSession::Contribution contribution;
    contribution.label = share.label;
    contribution.inputs = selection.inputs;
    if (selection.change > 0) {
        const QString change_address{freshAddress(error)};
        if (change_address.isEmpty()) {
            showError(error);
            return;
        }
        contribution.hasChange = true;
        contribution.changeAddress = change_address;
        contribution.changeAmount = selection.change;
    }
    if (!m_session.addContribution(contribution, error)) {
        showError(error);
        return;
    }
    setContributionLocked(contribution, /*lock=*/true);
    m_dirty = true;
    // The coins card now leads with "Reserved - ..." and the Reserve button has
    // become Release; repeating the same sentence under the page says it twice
    refreshAll();
}

void SharedMnCreateDialog::releaseCoins()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    const MnShareSession::Contribution* mine{myContribution()};
    if (mine == nullptr) return;
    const MnShareSession::Contribution removed{*mine};
    QString error;
    if (!m_session.removeContribution(removed.label, error)) {
        showError(error);
        return;
    }
    setContributionLocked(removed, /*lock=*/false);
    m_dirty = true;
    refreshAll();
}

void SharedMnCreateDialog::releaseReservedCoins()
{
    if (m_busy || !hasLockedContributedCoins()) return;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Release your coins?"));
    box.setText(tr("The coins you reserved for Session %1 become spendable again.").arg(m_session.sessionCode()));
    box.setInformativeText(tr("Everyone else's coins stay reserved. If you spend these, this registration can never "
                             "be broadcast and the others have to start over."));
    auto* release{box.addButton(tr("Release"), QMessageBox::DestructiveRole)};
    auto* cancel{box.addButton(tr("Cancel"), QMessageBox::RejectRole)};
    box.setDefaultButton(qobject_cast<QPushButton*>(cancel));
    box.exec();
    if (box.clickedButton() != release) return;
    unlockAllContributedCoins();
    refreshAll();
    showStatus(tr("Released — these coins can be spent again."));
}

bool SharedMnCreateDialog::hasLockedContributedCoins() const
{
    if (m_wallet_model == nullptr) return false;
    for (const auto& contribution : m_session.contributions()) {
        for (const auto& input : contribution.inputs) {
            if (m_wallet_model->wallet().isLockedCoin(COutPoint(uint256S(input.txid.toStdString()), input.vout))) {
                return true;
            }
        }
    }
    return false;
}

void SharedMnCreateDialog::refreshContributedCoinLocks()
{
    // Once the registration is broadcast the contributed coins are spent, so
    // keeping them in the wallet's persisted locked set only leaves the user
    // hunting for them in Coin Control
    if (m_session.stage() == MnShareSession::Stage::Broadcast) {
        unlockAllContributedCoins();
        return;
    }
    lockKnownContributedCoins();
}

void SharedMnCreateDialog::setContributionLocked(const MnShareSession::Contribution& contribution, bool lock)
{
    if (m_wallet_model == nullptr) return;
    for (const auto& input : contribution.inputs) {
        const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
        if (lock) {
            m_wallet_model->wallet().lockCoin(outpoint, /*write_to_db=*/true);
        } else {
            m_wallet_model->wallet().unlockCoin(outpoint);
        }
    }
}

void SharedMnCreateDialog::unlockAllContributedCoins()
{
    for (const auto& contribution : m_session.contributions()) {
        setContributionLocked(contribution, /*lock=*/false);
    }
}

void SharedMnCreateDialog::lockKnownContributedCoins()
{
    if (m_wallet_model == nullptr) return;
    for (const auto& contribution : m_session.contributions()) {
        for (const auto& input : contribution.inputs) {
            const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
            const auto known{m_wallet_model->wallet().getCoins({outpoint})};
            if (known.empty() || known.front().depth_in_main_chain < 0 || known.front().is_spent) continue;
            m_wallet_model->wallet().lockCoin(outpoint, /*write_to_db=*/true);
        }
    }
}

void SharedMnCreateDialog::checkSessionLiveness()
{
    m_dead_reason.clear();
    if (m_wallet_model == nullptr || m_session.stage() == MnShareSession::Stage::Broadcast) return;
    for (const auto& contribution : m_session.contributions()) {
        for (const auto& input : contribution.inputs) {
            const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
            Coin coin;
            if (m_node.getUnspentOutput(outpoint, coin)) continue;
            // Absence from the confirmed UTXO set proves nothing on its own:
            // another participant's unconfirmed coin looks exactly the same.
            // Only a coin this wallet tracks and reports spent is proof that
            // the recorded funding transaction can never confirm.
            const auto known{m_wallet_model->wallet().getCoins({outpoint})};
            if (known.empty() || known.front().depth_in_main_chain < 0 || !known.front().is_spent) continue;
            m_dead_reason = tr("A coin reserved for this session was spent. The registration can no longer be "
                               "completed — start over.");
            return;
        }
    }
}

const MnShareSession::Contribution* SharedMnCreateDialog::myContribution() const
{
    if (m_my_share < 0 || static_cast<size_t>(m_my_share) >= m_session.shares().size()) return nullptr;
    const QString label{m_session.shares()[m_my_share].label};
    if (label.isEmpty()) return nullptr;
    for (const auto& contribution : m_session.contributions()) {
        if (contribution.label == label) return &contribution;
    }
    return nullptr;
}

std::optional<CAmount> SharedMnCreateDialog::resolveInputValue(const MnShareSession::Input& input) const
{
    const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
    // The chainstate UTXO set still reports a coin as unspent while the
    // transaction spending it sits in the mempool, so this wallet's own view
    // decides for the coins it tracks. Other participants' coins are only
    // visible in the UTXO set.
    if (m_wallet_model != nullptr) {
        const auto known{m_wallet_model->wallet().getCoins({outpoint})};
        if (!known.empty() && known.front().depth_in_main_chain >= 0) {
            if (known.front().is_spent) return std::nullopt;
            return known.front().txout.nValue;
        }
    }
    Coin coin;
    if (m_node.getUnspentOutput(outpoint, coin)) return coin.out.nValue;
    return std::nullopt;
}

QString SharedMnCreateDialog::fundingCheck(bool& fatal) const
{
    fatal = false;
    const CAmount collateral{GetMnType(MnType::Regular).collat_amount};
    CAmount change_total{0};
    CAmount resolved{0};
    int unresolved{0};
    for (const auto& contribution : m_session.contributions()) {
        if (contribution.hasChange) change_total += contribution.changeAmount;
        for (const auto& input : contribution.inputs) {
            if (const auto value{resolveInputValue(input)}) {
                resolved += *value;
            } else {
                ++unresolved;
            }
        }
    }
    if (unresolved > 0) {
        const QString inputs{
            SharedMnPlural(unresolved, QT_TRANSLATE_NOOP("MnShareSession", "1 funding input"),
                           QT_TRANSLATE_NOOP("MnShareSession", "%1 funding inputs"))};
        return tr("%1 cannot be value-checked on this node yet. The inputs it can check total %2 of the %3 "
                  "collateral.")
            .arg(inputs, FormatAmount(m_wallet_model, resolved), FormatAmount(m_wallet_model, collateral));
    }
    const CAmount fee{resolved - collateral - change_total};
    if (fee <= 0) {
        fatal = true;
        return tr("The reserved coins total %1, which does not cover the %2 collateral plus %3 change and a network "
                  "fee. The registration could never be broadcast.")
            .arg(FormatAmount(m_wallet_model, resolved), FormatAmount(m_wallet_model, collateral),
                 FormatAmount(m_wallet_model, change_total));
    }
    if (fee > MAX_EXPECTED_FUNDING_FEE) {
        return tr("Inputs %1 − change %2 = %3 collateral + %4 network fee. That fee is unusually large; continue "
                  "only if it is intended.")
            .arg(FormatAmount(m_wallet_model, resolved), FormatAmount(m_wallet_model, change_total),
                 FormatAmount(m_wallet_model, collateral), FormatAmount(m_wallet_model, fee));
    }
    return tr("Inputs %1 − change %2 = %3 collateral + %4 network fee ✓")
        .arg(FormatAmount(m_wallet_model, resolved), FormatAmount(m_wallet_model, change_total),
             FormatAmount(m_wallet_model, collateral), FormatAmount(m_wallet_model, fee));
}

void SharedMnCreateDialog::inferMyShare()
{
    const auto& shares{m_session.shares()};
    if (m_my_share >= 0 && static_cast<size_t>(m_my_share) < shares.size()) return;
    if (m_wallet_model != nullptr) {
        for (size_t i = 0; i < shares.size(); ++i) {
            const CTxDestination dest{DecodeDestination(shares[i].ownerAddress.toStdString())};
            if (IsValidDestination(dest) && m_wallet_model->wallet().isSpendable(dest)) {
                m_my_share = static_cast<int>(i);
                return;
            }
        }
    }
    // Nothing of ours is in there yet: an invitation with exactly one row left
    // to fill in can only be about us.
    int candidate{-1};
    for (size_t i = 0; i < shares.size(); ++i) {
        if (!shares[i].ownerAddress.isEmpty()) continue;
        if (candidate >= 0) return;
        candidate = static_cast<int>(i);
    }
    m_my_share = candidate;
}

bool SharedMnCreateDialog::needsOwnApproval() const
{
    if (m_role != Role::Coordinator || !canSign()) return false;
    if (m_session.stage() != MnShareSession::Stage::Frozen && m_session.stage() != MnShareSession::Stage::Signing) {
        return false;
    }
    return m_my_share >= 0 && static_cast<size_t>(m_my_share) < m_session.shares().size() &&
           m_session.signatureFor(m_my_share).isEmpty();
}

bool SharedMnCreateDialog::coordinatedHere(const MnShareSession& session) const
{
    if (m_wallet_model == nullptr) return false;
    if (session.prepareWallet() != m_wallet_model->getWalletName()) return false;
    // The wallet name alone is not enough - two people can both be running the
    // default wallet - so the share the envelope names as the coordinator's
    // must be one this wallet can sign for. A participant's copy of the same
    // session names somebody else's share there.
    const QString coordinator{session.coordinatorLabel()};
    if (coordinator.isEmpty()) return false;
    for (const auto& share : session.shares()) {
        if (share.label != coordinator) continue;
        const CTxDestination dest{DecodeDestination(share.ownerAddress.toStdString())};
        return IsValidDestination(dest) && m_wallet_model->wallet().isSpendable(dest);
    }
    return false;
}

bool SharedMnCreateDialog::hasDetails(int share_index) const
{
    if (share_index < 0 || static_cast<size_t>(share_index) >= m_session.shares().size()) return false;
    const auto& share{m_session.shares()[share_index]};
    return !share.ownerAddress.isEmpty() && !share.refundAddress.isEmpty();
}

bool SharedMnCreateDialog::hasFunding(int share_index) const
{
    if (share_index < 0 || static_cast<size_t>(share_index) >= m_session.shares().size()) return false;
    const QString label{m_session.shares()[share_index].label};
    if (label.isEmpty()) return false;
    const auto& contributions{m_session.contributions()};
    return std::any_of(contributions.begin(), contributions.end(),
                       [&label](const auto& contribution) { return contribution.label == label; });
}

std::pair<int, int> SharedMnCreateDialog::fundingSignatureCount(int share_index) const
{
    if (share_index < 0 || static_cast<size_t>(share_index) >= m_session.shares().size()) return {0, 0};
    const QString label{m_session.shares()[share_index].label};
    const auto signature_map{InputSignatureMap(m_session.protxHex())};
    int done{0};
    int total{0};
    for (const auto& contribution : m_session.contributions()) {
        if (contribution.label != label) continue;
        for (const auto& input : contribution.inputs) {
            ++total;
            const auto it{signature_map.find(OutpointKey(input.txid, input.vout))};
            if (it != signature_map.end() && it->second) ++done;
        }
    }
    return {done, total};
}

bool SharedMnCreateDialog::allDetailsCollected() const
{
    const int shares{static_cast<int>(m_session.shares().size())};
    if (shares == 0) return false;
    for (int i = 0; i < shares; ++i) {
        if (!hasDetails(i) || !hasFunding(i)) return false;
    }
    return true;
}

bool SharedMnCreateDialog::allApproved() const
{
    const int shares{static_cast<int>(m_session.shares().size())};
    return shares > 0 && m_session.signedCount() == shares;
}

bool SharedMnCreateDialog::allFundingSigned() const
{
    return m_session.stage() == MnShareSession::Stage::FundingSigned ||
           m_session.stage() == MnShareSession::Stage::Broadcast;
}

bool SharedMnCreateDialog::runRpc(const QString& method, const UniValue& params, bool needs_unlock,
                                  ProTxResult& result)
{
    // UnlockContext is neither copyable nor movable: keep it on this stack
    // frame and wait in a nested event loop until the worker thread reports back
    std::unique_ptr<UnlockHolder> unlock;
    if (needs_unlock) {
        if (!canSign()) return false;
        unlock = std::make_unique<UnlockHolder>(*m_wallet_model);
        if (!unlock->ctx.isValid()) return false;
    }

    QEventLoop loop;
    connect(m_sender, &ProTxSender::finished, &loop, [&](const ProTxResult& r) {
        result = r;
        loop.quit();
    });
    if (!m_sender->execute(method, params, m_wallet_model)) return false;
    loop.exec();
    return true;
}

bool SharedMnCreateDialog::replaceSessionProTx(const QString& tx_hex, QString& error)
{
    UniValue json{m_session.toJson()};
    json.pushKV("protx", tx_hex.toStdString());
    return m_session.fromJson(json, error);
}

void SharedMnCreateDialog::reject()
{
    if (m_busy) return;
    if (m_finished || m_role == Role::Undecided || !m_dirty) {
        QDialog::reject();
        return;
    }
    const bool secret_unsaved{operatorSecretUnsaved()};
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    if (secret_unsaved) {
        // Saving is no way out here: the file carries only the operator public
        // key, so closing without the secret leaves a masternode nobody can run
        box.setWindowTitle(tr("Save the operator key first?"));
        box.setText(tr("The operator secret key exists nowhere but this window — the session file carries only its "
                       "public half. Close now and the masternode could never be run."));
    } else {
        box.setWindowTitle(tr("Keep this session for later?"));
        box.setText(tr("Saving keeps your coins reserved so you can carry on where you left off. Discarding releases "
                       "them; if the others are still waiting for you, they have to start over."));
    }
    auto* save{box.addButton(secret_unsaved ? tr("Save Operator Key") : tr("Save and Close"),
                             QMessageBox::AcceptRole)};
    auto* discard{box.addButton(tr("Discard and Release Coins"), QMessageBox::DestructiveRole)};
    box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(save);
    box.exec();
    if (box.clickedButton() == save) {
        if (secret_unsaved) {
            goToPage(PageSecret);
            return;
        }
        saveSession();
        if (m_dirty) return; // the save was cancelled
    } else if (box.clickedButton() == discard) {
        if (m_session.stage() != MnShareSession::Stage::Broadcast) unlockAllContributedCoins();
    } else {
        return;
    }
    QDialog::reject();
}
