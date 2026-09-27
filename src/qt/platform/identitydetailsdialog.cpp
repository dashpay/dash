// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/identitydetailsdialog.h>

#include <platform/helpers.h>
#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/optionsmodel.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/sharedmnwidgets.h>
#include <qt/walletmodel.h>
#include <util/strencodings.h>

#include <QAction>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

using MasternodeWidgetUtil::CARD_PADDING;
using MasternodeWidgetUtil::GROUP_SPACING;
using MasternodeWidgetUtil::ROW_SPACING;
using MasternodeWidgetUtil::TITLE_SPACING;
using Purpose = platform::IdentityPublicKey::Purpose;
using Level = platform::IdentityPublicKey::SecurityLevel;
using KeyType = platform::IdentityPublicKey::Type;

namespace {
constexpr int MIN_WIDTH{560};
//! Keys listed before the table scrolls.
constexpr int MAX_VISIBLE_KEYS{8};
constexpr char NONE_TEXT[]{"—"};

enum KeyColumn {
    KEY_ID = 0,
    KEY_PURPOSE,
    KEY_LEVEL,
    KEY_TYPE,
    KEY_LIMITED_TO,
    KEY_USED_BY,
    KEY_COLUMN_COUNT
};

QString PurposeText(Purpose purpose)
{
    switch (purpose) {
    case Purpose::AUTHENTICATION:
        return QObject::tr("Authentication");
    case Purpose::ENCRYPTION:
        return QObject::tr("Encryption");
    case Purpose::DECRYPTION:
        return QObject::tr("Decryption");
    case Purpose::TRANSFER:
        return QObject::tr("Transfer");
    case Purpose::VOTING:
        return QObject::tr("Voting");
    }
    return {};
}

QString LevelText(Level level)
{
    switch (level) {
    case Level::MASTER:
        return QObject::tr("Master");
    case Level::CRITICAL:
        return QObject::tr("Critical");
    case Level::HIGH:
        return QObject::tr("High");
    case Level::MEDIUM:
        return QObject::tr("Medium");
    }
    return {};
}

QString TypeText(KeyType type)
{
    switch (type) {
    case KeyType::ECDSA_SECP256K1:
        return QStringLiteral("ECDSA secp256k1");
    case KeyType::BLS12_381:
        return QStringLiteral("BLS12-381");
    case KeyType::ECDSA_HASH160:
        return QStringLiteral("ECDSA hash160");
    case KeyType::BIP13_SCRIPT_HASH:
        return QStringLiteral("BIP13 script hash");
    case KeyType::EDDSA_25519_HASH160:
        return QStringLiteral("EdDSA 25519 hash160");
    }
    return {};
}

QString LimitedToText(const platform::ContractBounds& bounds)
{
    const auto dashpay{platform::helpers::SystemContractId(platform::helpers::SystemContract::DASHPAY)};
    switch (bounds.kind) {
    case platform::ContractBounds::Kind::NONE:
        return QString::fromUtf8(NONE_TEXT);
    case platform::ContractBounds::Kind::SINGLE_CONTRACT_DOCUMENT_TYPE:
        if (bounds.contract_id == dashpay && bounds.document_type == "contactRequest") {
            return QObject::tr("DashPay contact requests");
        }
        break;
    case platform::ContractBounds::Kind::SINGLE_CONTRACT:
        if (bounds.contract_id == dashpay) return QObject::tr("DashPay");
        break;
    case platform::ContractBounds::Kind::CONTRACT_GROUP:
        break;
    }
    return QObject::tr("Another contract");
}

//! The dashboard's wording of a registration state.
QString StateText(const platform::IdentityRecord& record)
{
    using State = platform::IdentityRecord::State;
    if (record.AwaitsUsername()) return QObject::tr("No username yet");
    switch (record.state) {
    case State::REGISTERED:
        return QObject::tr("Registered");
    case State::CONTESTED_PENDING:
        return QObject::tr("Masternodes are voting on the username");
    case State::FAILED:
        return QObject::tr("Registration did not finish");
    case State::NEEDS_UNLOCK:
        return QObject::tr("Waiting for your passphrase");
    case State::NONE:
        return QString::fromUtf8(NONE_TEXT);
    case State::FUNDING_SENT:
    case State::FUNDING_LOCKED:
    case State::IDENTITY_BROADCAST:
    case State::IDENTITY_CONFIRMED:
    case State::PREORDER_BROADCAST:
    case State::PREORDER_WAIT:
    case State::DOMAIN_BROADCAST:
        break;
    }
    return QObject::tr("Registering: %1").arg(PlatformUi::registrationStepLine(record));
}

//! A card with a bold title and a form of rows.
QFormLayout* AddCard(QVBoxLayout* layout, QWidget* parent, const QString& title)
{
    auto* card{MasternodeWidgetUtil::makeCard(parent)};
    auto* card_layout{new QVBoxLayout(card)};
    card_layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
    card_layout->setSpacing(TITLE_SPACING);
    card_layout->addWidget(MasternodeWidgetUtil::makeBlockTitle(title, card));
    auto* form{new QFormLayout()};
    form->setVerticalSpacing(ROW_SPACING);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    card_layout->addLayout(form);
    layout->addWidget(card);
    return form;
}

//! One label column across the cards: every form's labels as wide as the
//! widest of them.
void AlignLabelColumns(QWidget* content)
{
    std::vector<QWidget*> labels;
    int width{0};
    for (QFormLayout* form : content->findChildren<QFormLayout*>()) {
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem* const item{form->itemAt(row, QFormLayout::LabelRole)};
            QWidget* const label{item ? item->widget() : nullptr};
            if (!label) continue;
            labels.push_back(label);
            width = std::max(width, label->sizeHint().width());
        }
    }
    for (QWidget* label : labels) {
        label->setMinimumWidth(width);
    }
}

QLabel* LoadingValue(QWidget* parent)
{
    auto* label{MasternodeWidgetUtil::makeValue({}, parent)};
    PlatformUi::setTextStyle(label, GUIUtil::ThemedStyle::TS_SECONDARY);
    return label;
}

void SetValue(QLabel* label, const QString& text)
{
    label->setText(text);
    PlatformUi::setTextStyle(label, GUIUtil::ThemedStyle::TS_PRIMARY);
}

//! A read failed before it filled the value: it is unknown, not loading.
void SetUnknown(QLabel* label)
{
    label->setText(QString::fromUtf8(NONE_TEXT));
    PlatformUi::setTextStyle(label, GUIUtil::ThemedStyle::TS_SECONDARY);
}

//! makeCopyableValue() with its Copy button secondary: Close is the
//! dialog's one filled button.
QWidget* CopyableValue(const QString& display, const QString& copy_text, QWidget* parent)
{
    QWidget* value{MasternodeWidgetUtil::makeCopyableValue(display, copy_text, parent)};
    for (QPushButton* button : value->findChildren<QPushButton*>()) {
        PlatformUi::makeSecondary(button);
        button->setAutoDefault(false);
    }
    return value;
}
} // namespace

IdentityDetailsDialog::IdentityDetailsDialog(PlatformService& service, bool paused, QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    m_service(service),
    m_paused(paused)
{
    setWindowTitle(tr("Identity details"));
    setMinimumWidth(MIN_WIDTH);
    const auto& rec{m_service.identityFlow().record()};

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 12, 24, 12);
    layout->setSpacing(GROUP_SPACING);
    m_busy = PlatformUi::makeBusyBar(this);
    layout->addWidget(m_busy);
    m_status = new PlatformUi::MessageLine(this);
    layout->addWidget(m_status);
    const auto body{MasternodeWidgetUtil::makeScrollBody(this, layout)};
    QWidget* const content{body.container};
    m_scroll = findChild<QScrollArea*>();

    // Identity.
    auto* identity{AddCard(body.layout, content, tr("Identity"))};
    const QString label{QString::fromStdString(rec.label)};
    identity->addRow(tr("Username"), label.isEmpty()
                                         ? MasternodeWidgetUtil::makeValue(QString::fromUtf8(NONE_TEXT), content)
                                         : CopyableValue(label, label, content));
    if (const auto id{m_service.myIdentityId()}) {
        const QString base58{PlatformUi::identityIdBase58(*id)};
        const QString hex{QString::fromStdString(HexStr(*id))};
        auto* value{CopyableValue(base58, base58, content)};
        if (auto* text{value->findChild<QLabel*>()}) {
            text->setToolTip(hex);
            text->setContextMenuPolicy(Qt::ActionsContextMenu);
            auto* copy_hex{new QAction(tr("Copy as hex"), text)};
            connect(copy_hex, &QAction::triggered, text, [hex] { GUIUtil::setClipboard(hex); });
            text->addAction(copy_hex);
        }
        identity->addRow(tr("Identity ID"), value);
    }
    identity->addRow(tr("Status"), MasternodeWidgetUtil::makeValue(StateText(rec), content));
    // Only a registration this wallet funded knows when it started; a
    // restored identity's record was written when it was found.
    if (rec.started_at > 0 && !rec.funding_txid.IsNull()) {
        identity->addRow(tr("Created"), MasternodeWidgetUtil::makeValue(GUIUtil::dateTimeStr(rec.started_at), content));
    }

    // Balance.
    auto* balance{AddCard(body.layout, content, tr("Balance on Dash Platform"))};
    m_balance = LoadingValue(content);
    balance->addRow(m_balance);
    balance->addRow(PlatformUi::makeHint(tr("Pays Dash Platform fees for changes such as your profile and contact "
                                            "requests. It is separate from your wallet balance, and Dash Core can't "
                                            "add to it yet."),
                                         content));
    connect(m_service.walletModel().getOptionsModel(), &OptionsModel::displayUnitChanged, this,
            &IdentityDetailsDialog::showBalance);

    // Profile.
    auto* profile{AddCard(body.layout, content, tr("Profile"))};
    m_display_name = LoadingValue(content);
    m_message = LoadingValue(content);
    profile->addRow(tr("Display name"), m_display_name);
    profile->addRow(tr("Public message"), m_message);

    // Keys: for support and for users of the identity in another app, so
    // they wait behind a toggle.
    m_keys_toggle = new QPushButton(tr("Show keys"), content);
    PlatformUi::makeSecondary(m_keys_toggle);
    m_keys_toggle->setAutoDefault(false);
    m_keys_toggle->setCheckable(true);
    body.layout->addWidget(m_keys_toggle, 0, Qt::AlignLeft);
    m_keys_card = MasternodeWidgetUtil::makeCard(content);
    auto* keys_layout{new QVBoxLayout(m_keys_card)};
    keys_layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
    keys_layout->setSpacing(TITLE_SPACING);
    keys_layout->addWidget(MasternodeWidgetUtil::makeBlockTitle(tr("Keys"), m_keys_card));
    keys_layout->addWidget(PlatformUi::makeHint(tr("The keys registered to your identity on Dash Platform. “Used by "
                                                   "this wallet” marks the ones this wallet holds the private keys "
                                                   "for."),
                                                m_keys_card));
    m_keys = new QTableWidget(0, KEY_COLUMN_COUNT, m_keys_card);
    m_keys->setHorizontalHeaderLabels(
        {tr("ID"), tr("Purpose"), tr("Security level"), tr("Type"), tr("Limited to"), tr("Used by this wallet")});
    m_keys->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_keys->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_keys->horizontalHeader()->setStretchLastSection(true);
    // Asks for every column, so the dialog can open the table in full.
    m_keys->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    m_keys->verticalHeader()->hide();
    m_keys->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_keys->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_keys->setSelectionMode(QAbstractItemView::SingleSelection);
    m_keys->setContextMenuPolicy(Qt::CustomContextMenu);
    keys_layout->addWidget(m_keys);
    body.layout->addWidget(m_keys_card);
    m_keys_card->hide();
    connect(m_keys_toggle, &QPushButton::toggled, this, [this](bool shown) {
        m_keys_card->setVisible(shown);
        m_keys_toggle->setText(shown ? tr("Hide keys") : tr("Show keys"));
        if (!shown) return;
        fitToContent();
        m_scroll->ensureWidgetVisible(m_keys_card);
    });
    connect(m_keys, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const auto* item{m_keys->itemAt(pos)};
        if (!item || !m_identity) return;
        const int row{item->row()};
        if (row < 0 || row >= static_cast<int>(m_identity->public_keys.size())) return;
        QMenu menu(this);
        const QString key_hex{QString::fromStdString(HexStr(m_identity->public_keys[row].data))};
        connect(menu.addAction(tr("Copy public key (hex)")), &QAction::triggered, this,
                [key_hex] { GUIUtil::setClipboard(key_hex); });
        menu.exec(m_keys->viewport()->mapToGlobal(pos));
    });
    body.layout->addStretch();
    AlignLabelColumns(content);

    auto* buttons = new QHBoxLayout();
    auto* close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_status, &PlatformUi::MessageLine::actionClicked, this, &IdentityDetailsDialog::load);
    connect(&m_service, &PlatformService::myIdentityLoaded, this, &IdentityDetailsDialog::onIdentity);
    connect(&m_service, &PlatformService::myIdentityFailed, this, [this](const QString& error, const QString& details) {
        if (!m_identity_pending) return;
        m_identity_pending = false;
        setBusy(m_profile_pending);
        if (!m_identity) SetUnknown(m_balance);
        m_status->setMessage(PlatformUi::MessageLine::Severity::Error, error, details);
        m_status->setAction(tr("Try again"));
    });
    connect(&m_service, &PlatformService::profileLoaded, this, &IdentityDetailsDialog::onProfile);
    connect(&m_service, &PlatformService::profileLoadFailed, this,
            [this](const QString& identity_hex, const QString& error, const QString& details) {
                const auto id{m_service.myIdentityId()};
                if (!id || QString::fromStdString(HexStr(*id)) != identity_hex || !m_profile_pending) return;
                m_profile_pending = false;
                setBusy(m_identity_pending);
                if (!m_have_profile) {
                    for (QLabel* value : {m_display_name, m_message}) {
                        SetUnknown(value);
                    }
                }
                m_status->setMessage(PlatformUi::MessageLine::Severity::Error, error, details);
                m_status->setAction(tr("Try again"));
            });

    GUIUtil::updateFonts();
    GUIUtil::disableMacFocusRect(this);
    SharedMnFitWrappedLabels(this);
    SharedMnSizeFromContent(this, MIN_WIDTH);
    setTabOrder(m_keys_toggle, close);
    close->setFocus();
    load();
}

void IdentityDetailsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::StyleChange) PlatformUi::restyle(this);
}

void IdentityDetailsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!event->spontaneous()) fitToContent();
}

void IdentityDetailsDialog::fitToContent()
{
    QWidget* const content{m_scroll->widget()};
    content->layout()->activate();
    // Headless platforms report no usable geometry; only a real screen caps
    // the dialog, at the 85% SharedMnSizeFromContent() allows.
    QSize cap{QWIDGETSIZE_MAX, QWIDGETSIZE_MAX};
    if (const QScreen* const screen{this->screen()}) {
        const QSize available{screen->availableGeometry().size()};
        if (available.width() >= 640 && available.height() >= 480) cap = available * 85 / 100;
    }
    const int grow_width{std::max(0, content->sizeHint().width() - m_scroll->viewport()->width())};
    const int new_width{std::max(width(), std::min(width() + grow_width, cap.width()))};
    const int viewport_width{m_scroll->viewport()->width() + new_width - width()};
    const int wanted{content->hasHeightForWidth() ? content->heightForWidth(viewport_width)
                                                  : content->sizeHint().height()};
    const int grow_height{std::max(0, wanted - m_scroll->viewport()->height())};
    resize(new_width, std::max(height(), std::min(height() + grow_height, cap.height())));
}

void IdentityDetailsDialog::setBusy(bool busy) { m_busy->setVisible(busy); }

void IdentityDetailsDialog::load()
{
    const auto id{m_service.myIdentityId()};
    if (!id) return;
    // Every read discloses the identity: none while DashPay is paused.
    if (m_paused || !m_service.haveEndpoints()) {
        for (QLabel* value : {m_balance, m_display_name, m_message}) {
            SetUnknown(value);
        }
        m_status->setMessage(PlatformUi::MessageLine::Severity::Info,
                             tr("DashPay is paused, so only what this wallet knows is shown."));
        return;
    }
    m_status->clear();
    m_identity_pending = true;
    m_profile_pending = true;
    setBusy(true);
    for (QLabel* value : {m_balance, m_display_name, m_message}) {
        if (value->text().isEmpty()) value->setText(tr("Loading…"));
    }
    m_service.loadMyIdentity();
    m_service.loadProfile(*id);
}

void IdentityDetailsDialog::showBalance()
{
    if (!m_identity || m_service.stopped()) return;
    SetValue(m_balance, PlatformUi::formatPlatformBalance(m_service.walletModel().getOptionsModel()->getDisplayUnit(),
                                                          m_identity->balance));
}

void IdentityDetailsDialog::onIdentity(const platform::Identity& identity)
{
    m_identity_pending = false;
    setBusy(m_profile_pending);
    m_identity = identity;
    showBalance();

    const auto& rec{m_service.identityFlow().record()};
    m_keys->setRowCount(0);
    for (const auto& key : identity.public_keys) {
        const int row{m_keys->rowCount()};
        m_keys->insertRow(row);
        QString used_by{QString::fromUtf8(NONE_TEXT)};
        if (key.id == rec.auth_key_id) used_by = tr("Signing");
        if (key.id == rec.encryption_key_id) used_by = tr("Encryption");
        if (key.id == rec.decryption_key_id) used_by = tr("Decryption");
        const QStringList cells{key.disabled_at ? tr("%1 (disabled)").arg(key.id) : QString::number(key.id),
                                PurposeText(key.purpose),
                                LevelText(key.security_level),
                                TypeText(key.type),
                                LimitedToText(key.contract_bounds),
                                used_by};
        for (int column = 0; column < KEY_COLUMN_COUNT; ++column) {
            auto* item{new QTableWidgetItem(cells[column])};
            if (key.disabled_at) item->setForeground(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::UNCONFIRMED));
            m_keys->setItem(row, column, item);
        }
    }
    // As tall as its rows: no empty block under the last key.
    const int rows{std::clamp(m_keys->rowCount(), 1, MAX_VISIBLE_KEYS)};
    m_keys->setFixedHeight(m_keys->horizontalHeader()->sizeHint().height() +
                           rows * m_keys->verticalHeader()->defaultSectionSize() + 2 * m_keys->frameWidth());
    if (m_keys_card->isVisible()) fitToContent();
}

void IdentityDetailsDialog::onProfile(const QString& identity_hex, const QString& display_name,
                                      const QString& public_message, quint64 revision)
{
    const auto id{m_service.myIdentityId()};
    if (!id || QString::fromStdString(HexStr(*id)) != identity_hex) return;
    // The dashboard reads the same profile; any proved answer is current.
    m_profile_pending = false;
    m_have_profile = true;
    setBusy(m_identity_pending);
    const bool none{revision == 0 && display_name.isEmpty() && public_message.isEmpty()};
    const QString dash{QString::fromUtf8(NONE_TEXT)};
    SetValue(m_display_name, none ? tr("No profile yet") : display_name.isEmpty() ? dash : display_name);
    SetValue(m_message, public_message.isEmpty() ? dash : public_message);
}
