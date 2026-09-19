// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/masternodewidgets.h>

#include <bls/bls.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <script/standard.h>
#include <support/cleanse.h>
#include <util/strencodings.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/walletmodel.h>

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace MasternodeWidgetUtil {

QStringList tokenizeEndpointList(const QString& input)
{
    static const QRegularExpression separator{"[,\\s]+"};
    return input.split(separator, Qt::SkipEmptyParts);
}

bool isP2PKHAddress(const QString& address)
{
    const CTxDestination dest{DecodeDestination(address.trimmed().toStdString())};
    return std::holds_alternative<PKHash>(dest);
}

bool isP2PKHorP2SHAddress(const QString& address)
{
    const CTxDestination dest{DecodeDestination(address.trimmed().toStdString())};
    return std::holds_alternative<PKHash>(dest) || std::holds_alternative<ScriptHash>(dest);
}

QLabel* makeTitle(const QString& text, QWidget* parent, double point_size)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    GUIUtil::setFont({label}, GUIUtil::FontWeight::Bold, point_size);
    return label;
}

QLabel* makeHint(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_SECONDARY));
    return label;
}

QLabel* makeValue(const QString& text, QWidget* parent, bool monospace)
{
    auto* label{new QLabel(text, parent)};
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (monospace) label->setFont(GUIUtil::fixedPitchFont());
    return label;
}

QWidget* makeCopyableValue(const QString& display, const QString& copy_text, QWidget* parent)
{
    auto* row_widget{new QWidget(parent)};
    auto* row{new QHBoxLayout(row_widget)};
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(TITLE_SPACING);
    auto* value{makeValue(display, row_widget, /*monospace=*/true)};
    value->setToolTip(copy_text);
    row->addWidget(value, /*stretch=*/1);
    auto* copy{new QPushButton(QObject::tr("Copy"), row_widget)};
    copy->setToolTip(QObject::tr("Copy to clipboard"));
    QObject::connect(copy, &QPushButton::clicked, copy, [copy_text] { GUIUtil::setClipboard(copy_text); });
    row->addWidget(copy, /*stretch=*/0, Qt::AlignTop);
    return row_widget;
}

QFrame* makeCard(QWidget* parent)
{
    auto* card{new QFrame(parent)};
    card->setObjectName("mnCard");
    return card;
}

OptionCard makeOptionCard(QWidget* parent, QWidget* header, const QString& hint)
{
    OptionCard ret;
    ret.card = makeCard(parent);
    auto* card_layout{new QVBoxLayout(ret.card)};
    card_layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
    card_layout->setSpacing(TITLE_SPACING);
    card_layout->addWidget(header);
    if (!hint.isEmpty()) {
        auto* hint_row{new QHBoxLayout()};
        hint_row->setContentsMargins(BODY_INDENT, 0, 0, 0);
        hint_row->addWidget(makeHint(hint, ret.card), /*stretch=*/1);
        card_layout->addLayout(hint_row);
    }
    ret.body = new QWidget(ret.card);
    ret.body_layout = new QVBoxLayout(ret.body);
    ret.body_layout->setContentsMargins(BODY_INDENT, 0, 0, 0);
    ret.body_layout->setSpacing(ROW_SPACING);
    card_layout->addWidget(ret.body);
    return ret;
}

} // namespace MasternodeWidgetUtil

FeeSourcePicker::FeeSourcePicker(QWidget* parent) :
    QComboBox(parent)
{
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(40);
}

void FeeSourcePicker::setWalletModel(WalletModel* wallet_model)
{
    m_wallet_model = wallet_model;
    refresh();
}

void FeeSourcePicker::setAutomaticOption(const QString& label)
{
    if (m_automatic_label == label) return;
    m_automatic_label = label;
    refresh();
}

void FeeSourcePicker::setMinimumBalance(CAmount minimum)
{
    if (m_minimum_balance == minimum) return;
    m_minimum_balance = minimum;
    refresh();
}

void FeeSourcePicker::setExcludedOutpoint(std::optional<COutPoint> outpoint)
{
    if (m_excluded_outpoint == outpoint) return;
    m_excluded_outpoint = std::move(outpoint);
    refresh();
}

void FeeSourcePicker::refresh()
{
    clear();
    if (!m_automatic_label.isEmpty()) addItem(m_automatic_label, QVariant{});
    if (m_wallet_model == nullptr) return;

    // Group each coin under its own scriptPubKey destination: the RPCs'
    // FundSpecialTx only selects coins paying exactly the chosen address, while
    // listCoins() groups change outputs under their parent non-change address,
    // which would overstate what a protx call can actually spend.
    std::map<CTxDestination, CAmount> balances;
    for (const auto& [dest, coins] : m_wallet_model->wallet().listCoins()) {
        for (const auto& [outpoint, txout] : coins) {
            if (m_excluded_outpoint && outpoint == *m_excluded_outpoint) continue;
            if (txout.is_spent || txout.depth_in_main_chain < 0) continue;
            if (m_wallet_model->wallet().isLockedCoin(outpoint)) continue;
            if (!m_wallet_model->wallet().isSpendable(txout.txout.scriptPubKey)) continue;
            CTxDestination coin_dest;
            if (!ExtractDestination(txout.txout.scriptPubKey, coin_dest)) continue;
            balances[coin_dest] += txout.txout.nValue;
        }
    }

    std::vector<std::pair<QString, CAmount>> entries;
    for (const auto& [dest, total] : balances) {
        if (total <= 0 || total < m_minimum_balance) continue;
        entries.emplace_back(QString::fromStdString(EncodeDestination(dest)), total);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

    const auto unit{m_wallet_model->getOptionsModel()->getDisplayUnit()};
    for (const auto& [address, balance] : entries) {
        addItem(QString("%1 (%2)").arg(address, BitcoinUnits::formatWithUnit(unit, balance, /*plussign=*/false,
                                                                             BitcoinUnits::SeparatorStyle::ALWAYS)),
                address);
    }
    if (count() > 0) setCurrentIndex(0);
}

QString FeeSourcePicker::selectedAddress() const
{
    return currentData().toString();
}

OperatorKeyWidget::OperatorKeyWidget(QWidget* parent) :
    QWidget(parent)
{
    using namespace MasternodeWidgetUtil;

    // Generate the key pair up front so toggling the radios never discards a
    // key the user may already have seen.
    CBLSSecretKey secret_key;
    secret_key.MakeNewKey();
    auto generated_secret{secret_key.ToByteVector(/*specificLegacyScheme=*/false)};
    m_generated_secret.assign(generated_secret.begin(), generated_secret.end());
    if (!generated_secret.empty()) memory_cleanse(generated_secret.data(), generated_secret.size());
    m_generated_public = QString::fromStdString(secret_key.GetPublicKey().ToString(/*specificLegacyScheme=*/false));

    m_generate_radio = new QRadioButton(tr("Generate a new operator key"), this);
    m_generate_radio->setChecked(true);
    auto generate_card{makeOptionCard(this, m_generate_radio,
                                      tr("The secret key is shown and confirmed before registering, then kept "
                                         "nowhere else."))};
    m_generate_body = generate_card.body;
    // The key is 96 hex characters: it gets its own full-width line, otherwise it
    // wraps against the edge of whatever sits beside it
    generate_card.body_layout->addWidget(makeHint(tr("Public key"), m_generate_body));
    generate_card.body_layout->addWidget(makeValue(chunked(m_generated_public), m_generate_body,
                                                   /*monospace=*/true));

    m_existing_radio = new QRadioButton(tr("Use an existing operator public key"), this);
    auto existing_card{makeOptionCard(this, m_existing_radio,
                                      tr("For a hosted node: the operator keeps the secret key and gives you the "
                                         "public key."))};
    m_existing_body = existing_card.body;
    m_existing_edit = new QValidatedLineEdit(m_existing_body);
    m_existing_edit->setPlaceholderText(tr("Operator BLS public key (96 hexadecimal characters, basic scheme)"));
    m_existing_edit->setEnabled(false);
    existing_card.body_layout->addWidget(m_existing_edit);

    // The radios live in separate cards, so auto-exclusivity by parent no
    // longer applies and a button group has to keep them exclusive.
    auto* group{new QButtonGroup(this)};
    group->addButton(m_generate_radio);
    group->addButton(m_existing_radio);

    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(TITLE_SPACING);
    layout->addWidget(generate_card.card);
    layout->addWidget(existing_card.card);

    connect(m_generate_radio, &QRadioButton::toggled, this, &OperatorKeyWidget::updateState);
    connect(m_existing_radio, &QRadioButton::toggled, this, &OperatorKeyWidget::updateState);
    connect(m_existing_edit, &QLineEdit::textChanged, this, &OperatorKeyWidget::updateState);
    updateState();
}

OperatorKeyWidget::~OperatorKeyWidget()
{
    clearSecret(m_generated_secret);
}

void OperatorKeyWidget::updateState()
{
    const Mode current{mode()};
    m_generate_body->setVisible(current == Mode::GenerateOnly);
    m_existing_body->setVisible(current == Mode::Existing);
    m_existing_edit->setEnabled(current == Mode::Existing);
    if (current == Mode::Existing) {
        const QString text{m_existing_edit->text().trimmed()};
        // Leave the neutral style while empty; only flag actual invalid input
        m_existing_edit->setValid(text.isEmpty() || isValid());
    }
    Q_EMIT changed();
}

OperatorKeyWidget::Mode OperatorKeyWidget::mode() const
{
    if (m_existing_radio->isChecked()) return Mode::Existing;
    return Mode::GenerateOnly;
}

QString OperatorKeyWidget::publicKeyHex() const
{
    switch (mode()) {
    case Mode::GenerateOnly:
        return m_generated_public;
    case Mode::Existing:
        return isValid() ? m_existing_edit->text().trimmed() : QString();
    }
    return {};
}

QString OperatorKeyWidget::secretHex() const
{
    switch (mode()) {
    case Mode::GenerateOnly:
        return secretDisplay(m_generated_secret);
    case Mode::Existing:
        return {};
    }
    return {};
}

QString OperatorKeyWidget::secretDisplay(Span<const unsigned char> secret)
{
    std::string encoded{HexStr(secret)};
    QString display{QString::fromStdString(encoded)};
    if (!encoded.empty()) memory_cleanse(encoded.data(), encoded.size());
    return display;
}

void OperatorKeyWidget::clearSecret(SecureVector& secret)
{
    if (!secret.empty()) memory_cleanse(secret.data(), secret.size());
    secret.clear();
    secret.shrink_to_fit();
}

bool OperatorKeyWidget::hasGeneratedSecret() const
{
    return mode() == Mode::GenerateOnly;
}

bool OperatorKeyWidget::isValid() const
{
    if (hasGeneratedSecret()) return !m_generated_public.isEmpty();
    CBLSPublicKey pubkey;
    return pubkey.SetHexStr(m_existing_edit->text().trimmed().toStdString(), /*specificLegacyScheme=*/false);
}
