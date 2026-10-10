// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformui.h>

#include <platform/helpers.h>
#include <qt/masternodewidgets.h>
#include <qt/sharedmnwidgets.h>

#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace PlatformUi {
namespace {
using MasternodeWidgetUtil::TITLE_SPACING;
using platform::StatusKind;

constexpr char TEXT_STYLE_PROPERTY[]{"platformTextStyle"};
constexpr char TEXT_COLOR_PROPERTY[]{"platformTextColor"};
constexpr int DETAILS_MAX_LINES{5};
constexpr int TRANSIENT_MESSAGE_MS{5'000};
constexpr int MESSAGE_ICON_SIZE{16};

void ApplyTextStyle(QLabel* label)
{
    const QVariant style{label->property(TEXT_STYLE_PROPERTY)};
    if (style.isValid()) {
        label->setStyleSheet(GUIUtil::getThemedStyleQString(static_cast<GUIUtil::ThemedStyle>(style.toInt())));
        return;
    }
    const QVariant color{label->property(TEXT_COLOR_PROPERTY)};
    if (color.isValid()) {
        label->setStyleSheet(QStringLiteral("QLabel { color: %1; }")
                                 .arg(GUIUtil::getThemedQColor(static_cast<GUIUtil::ThemedColor>(color.toInt())).name()));
    }
}

bool IsFlowContext(Context context) { return context == Context::IDENTITY_CREATE || context == Context::NAME_REGISTER; }

QString KindName(StatusKind kind)
{
    switch (kind) {
    case StatusKind::OK:
        return QStringLiteral("OK");
    case StatusKind::PROVEN_ABSENT:
        return QStringLiteral("PROVEN_ABSENT");
    case StatusKind::ALREADY_EXISTS:
        return QStringLiteral("ALREADY_EXISTS");
    case StatusKind::CONSENSUS:
        return QStringLiteral("CONSENSUS");
    case StatusKind::UNAVAILABLE:
        return QStringLiteral("UNAVAILABLE");
    case StatusKind::REJECTED:
        return QStringLiteral("REJECTED");
    case StatusKind::UNSUPPORTED_PROTOCOL_VERSION:
        return QStringLiteral("UNSUPPORTED_PROTOCOL_VERSION");
    case StatusKind::INTERNAL:
        return QStringLiteral("INTERNAL");
    }
    return {};
}

//! rs-dpp names of the consensus codes the texts below single out
//! (rs-dpp/src/errors/consensus/codes.rs).
QString ConsensusName(uint32_t code)
{
    switch (code) {
    case 10000:
        return QStringLiteral("UnsupportedVersionError");
    case 10003:
        return QStringLiteral("UnsupportedProtocolVersionError");
    case 10004:
        return QStringLiteral("IncompatibleProtocolVersionError");
    case 10405:
        return QStringLiteral("InvalidDocumentTransitionIdError");
    case 10414:
        return QStringLiteral("NonceOutOfBoundsError");
    case 10418:
        return QStringLiteral("ContestedDocumentsTemporarilyNotAllowedError");
    case 10504:
        return QStringLiteral("IdentityAssetLockTransactionOutPointAlreadyConsumedError");
    case 10506:
        return QStringLiteral("InvalidAssetLockProofCoreChainHeightError");
    case 10513:
        return QStringLiteral("InvalidInstantAssetLockProofSignatureError");
    case 10514:
        return QStringLiteral("InvalidIdentityAssetLockProofChainLockValidationError");
    case 10530:
        return QStringLiteral("IdentityAssetLockTransactionOutPointNotEnoughBalanceError");
    case 10602:
        return QStringLiteral("StateTransitionMaxSizeExceededError");
    case 20000:
        return QStringLiteral("IdentityNotFoundError");
    case 20002:
        return QStringLiteral("InvalidStateTransitionSignatureError");
    case 20003:
        return QStringLiteral("MissingPublicKeyError");
    case 20004:
        return QStringLiteral("InvalidSignaturePublicKeySecurityLevelError");
    case 20006:
        return QStringLiteral("PublicKeyIsDisabledError");
    case 20007:
        return QStringLiteral("PublicKeySecurityLevelNotMetError");
    case 30000:
        return QStringLiteral("BalanceIsNotEnoughError");
    case 40105:
        return QStringLiteral("DuplicateUniqueIndexError");
    case 40106:
        return QStringLiteral("InvalidDocumentRevisionError");
    case 40110:
        return QStringLiteral("DocumentContestCurrentlyLockedError");
    case 40111:
        return QStringLiteral("DocumentContestNotJoinableError");
    case 40112:
        return QStringLiteral("DocumentContestIdentityAlreadyContestantError");
    case 40114:
        return QStringLiteral("DocumentContestNotPaidForError");
    case 40200:
        return QStringLiteral("IdentityAlreadyExistsError");
    case 40204:
        return QStringLiteral("InvalidIdentityNonceError");
    case 40208:
        return QStringLiteral("IdentityPublicKeyIsDisabledError");
    case 40210:
        return QStringLiteral("IdentityInsufficientBalanceError");
    case 40400:
        return QStringLiteral("PrefundedSpecializedBalanceInsufficientError");
    case 40500:
        return QStringLiteral("DataTriggerConditionError");
    }
    return {};
}

QString UnsupportedVersionText()
{
    return QObject::tr("Dash Platform has been upgraded. You can still see your DashPay data, but update Dash Core to "
                       "make changes.");
}

//! The text for a consensus code, or empty to fall back to the kind's text.
QString ConsensusText(uint32_t code, Context context, const QString& subject)
{
    switch (code) {
    case 10405:
        return QObject::tr("Dash Core built this request in a way Dash Platform does not accept. This is a problem in "
                           "Dash Core, not something you did. Your funds are safe.");
    case 10414:
    case 40204:
        return QObject::tr("Another change from your identity was being processed at the same time. Try again.");
    case 30000:
    case 40210:
        return QObject::tr("Your balance on Dash Platform is too low for this. Dash Core can't add to it yet.");
    case 20002:
    case 20003:
    case 20004:
    case 20006:
    case 20007:
    case 40208:
        return QObject::tr("Dash Platform did not accept this wallet's key for your identity. If you also use this "
                           "identity in another app, a key may have been changed there.");
    case 10000:
    case 10003:
    case 10004:
        return UnsupportedVersionText();
    case 20000:
        return QObject::tr(
            "Your identity was not found on Dash Platform. If you just created it, wait a minute and try "
            "again.");
    }
    switch (context) {
    case Context::NAME_REGISTER:
        switch (code) {
        case 40105:
            return QObject::tr("Someone registered “%1” moments ago. Choose another username.").arg(subject);
        case 40500:
            return QObject::tr(
                "Dash Platform refused this username. It may be reserved, or the name reservation was not "
                "visible yet. Try again; if it happens again, choose another username.");
        case 40110:
            return QObject::tr("Masternodes have locked “%1”, so nobody can register it. Choose another username.").arg(subject);
        case 40111:
            return QObject::tr("The vote on “%1” is too far along to join. Choose another username.").arg(subject);
        case 40114:
        case 40400:
            return QObject::tr(
                "Your balance on Dash Platform is too low for the premium-name vote. Choose a username that is not "
                "premium.");
        case 10418:
            return QObject::tr(
                "Premium usernames can't be registered on Dash Platform right now. Choose one that is not "
                "premium, or try again later.");
        }
        break;
    case Context::IDENTITY_CREATE:
        switch (code) {
        case 10504:
        case 40200:
            return QObject::tr("Your identity already exists on Dash Platform. Dash Core will continue from it.");
        case 10506:
        case 10513:
        case 10514:
            return QObject::tr("Dash Platform has not seen your funding payment confirmed yet. Dash Core will retry "
                               "automatically.");
        case 10530:
            return QObject::tr("The funding payment is too small to create an identity.");
        }
        break;
    case Context::PROFILE:
        switch (code) {
        case 40106:
            return QObject::tr(
                "Your profile was changed from another wallet at the same time. Close this window and edit "
                "it again.");
        case 10602:
            return QObject::tr("Your profile is too large. Shorten the public message.");
        }
        break;
    case Context::CONTACT_REQUEST:
        if (code == 40105) return QObject::tr("You have already sent a contact request to %1.").arg(subject);
        break;
    case Context::READ:
    case Context::SEARCH:
    case Context::NAME_CHECK:
    case Context::CONTACT_ACCEPT:
    case Context::PAYMENT_LOOKUP:
        break;
    }
    return {};
}

//! One disc of the given fill, with an optional centred glyph.
QPixmap PaintDisc(const QColor& fill, const QString& glyph, const QColor& glyph_color, int size, qreal dpr)
{
    QPixmap pixmap{qRound(size * dpr), qRound(size * dpr)};
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter{&pixmap};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(fill);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(0, 0, size, size);
    if (!glyph.isEmpty()) {
        painter.setPen(glyph_color);
        QFont font;
        font.setBold(true);
        font.setPixelSize(size / 2);
        painter.setFont(font);
        painter.drawText(QRect{0, 0, size, size}, Qt::AlignCenter, glyph);
    }
    return pixmap;
}
} // namespace

void makeSecondary(QPushButton* button, bool secondary)
{
    if (secondary) {
        SharedMnMakeSecondary(button);
    } else {
        button->setProperty("mnSecondary", false);
    }
    // The themes read the property when the button is polished.
    button->style()->unpolish(button);
    button->style()->polish(button);
}

QProgressBar* makeBusyBar(QWidget* parent)
{
    auto* bar{new QProgressBar(parent)};
    bar->setRange(0, 0);
    bar->setTextVisible(false);
    bar->setMaximumHeight(4);
    bar->setVisible(false);
    return bar;
}

QLabel* makeHint(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    setTextStyle(label, GUIUtil::ThemedStyle::TS_SECONDARY);
    return label;
}

void setTextStyle(QLabel* label, GUIUtil::ThemedStyle style)
{
    label->setProperty(TEXT_COLOR_PROPERTY, QVariant{});
    label->setProperty(TEXT_STYLE_PROPERTY, static_cast<int>(style));
    ApplyTextStyle(label);
}

void setTextColor(QLabel* label, GUIUtil::ThemedColor color)
{
    label->setProperty(TEXT_STYLE_PROPERTY, QVariant{});
    label->setProperty(TEXT_COLOR_PROPERTY, static_cast<int>(color));
    ApplyTextStyle(label);
}

void restyle(QWidget* root)
{
    for (QLabel* const label : root->findChildren<QLabel*>()) {
        ApplyTextStyle(label);
    }
}

std::optional<int64_t> StaleBlockTimeMs(const platform::Status& status)
{
    // The pinned dash-platform-cxx shell folds dash_sdk::Error::StaleNode into
    // REJECTED with only the SDK's Display text, so the SDK's
    // StaleNodeError::Time wording (rs-sdk/src/error.rs) is the one signal:
    // "received invalid time: expected <local>ms, received <block> ms,
    // tolerance <t> ms; try another server". Match it anywhere, as a wrapping
    // error may prefix it. Only a block time behind the local clock is a
    // halted chain; one ahead of it points at this computer's clock instead.
    if (status.kind != StatusKind::REJECTED) return std::nullopt;
    static const QRegularExpression pattern{
        QStringLiteral(R"(received invalid time: expected (\d+) ?ms, received (\d+) ?ms)")};
    const QRegularExpressionMatch match{pattern.match(QString::fromStdString(status.message))};
    if (!match.hasMatch()) return std::nullopt;
    bool expected_ok{false}, received_ok{false};
    const qlonglong expected_ms{match.captured(1).toLongLong(&expected_ok)};
    const qlonglong received_ms{match.captured(2).toLongLong(&received_ok)};
    if (!expected_ok || !received_ok || received_ms <= 0 || received_ms >= expected_ms) return std::nullopt;
    return int64_t{received_ms};
}

UserError Describe(const platform::Status& status, Context context, const QString& operation, const QString& subject)
{
    UserError out;
    switch (status.kind) {
    case StatusKind::OK:
    case StatusKind::PROVEN_ABSENT:
    case StatusKind::ALREADY_EXISTS:
        return out;
    case StatusKind::CONSENSUS:
        out.text = ConsensusText(status.consensus_code, context, subject);
        if (out.text.isEmpty()) out.text = QObject::tr("Dash Platform did not accept this change.");
        break;
    case StatusKind::UNAVAILABLE:
        // Only the registration steps are retried by Dash Core on its own.
        out.text = IsFlowContext(context)
                       ? QObject::tr(
                             "Dash Core could not reach Dash Platform. It will keep trying; you can also choose "
                             "Try again.")
                       : QObject::tr("Dash Platform can't be reached right now. Try again in a moment.");
        break;
    case StatusKind::REJECTED:
        if (const auto block_ms{StaleBlockTimeMs(status)}) {
            out.text = QObject::tr("Dash Platform hasn't produced a new block since %1, so its answers can't be "
                                   "confirmed as current. DashPay will work again when Dash Platform resumes.")
                           .arg(GUIUtil::dateTimeStr(QDateTime::fromMSecsSinceEpoch(*block_ms)));
            break;
        }
        out.text = QObject::tr("Dash Core could not verify Dash Platform's answer, so it was not used. This is usually "
                               "temporary. Try again in a minute.");
        break;
    case StatusKind::UNSUPPORTED_PROTOCOL_VERSION:
        out.text = UnsupportedVersionText();
        break;
    case StatusKind::INTERNAL:
        out.text = QObject::tr("Something went wrong inside Dash Core. Please report it and include the details.");
        break;
    }

    out.details = Details(&status, operation, QDateTime::currentSecsSinceEpoch());
    return out;
}

QString Details(const platform::Status* status, const QString& operation, int64_t time)
{
    QStringList lines;
    if (!operation.isEmpty()) lines << QStringLiteral("Operation: %1").arg(operation);
    if (status) {
        QString result{KindName(status->kind)};
        if (status->kind == StatusKind::CONSENSUS) {
            const QString name{ConsensusName(status->consensus_code)};
            result += name.isEmpty() ? QStringLiteral(" (consensus code %1)").arg(status->consensus_code)
                                     : QStringLiteral(" (consensus code %1 %2)").arg(status->consensus_code).arg(name);
        }
        lines << QStringLiteral("Result: %1").arg(result);
        lines << QStringLiteral("Message: %1").arg(QString::fromStdString(status->message));
    }
    lines << QStringLiteral("Time: %1").arg(QDateTime::fromSecsSinceEpoch(time, Qt::UTC).toString(Qt::ISODate));
    return lines.join(QLatin1Char('\n'));
}

std::array<QColor, 6> avatarPalette()
{
    const QColor blue{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BLUE)};
    const QColor green{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::GREEN)};
    const QColor orange{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::ORANGE)};
    return {blue, blue.darker(130), blue.lighter(125), green, green.darker(125), orange.darker(110)};
}

QPixmap avatarPixmap(const QString& username, const QString& display_name, int size, qreal dpr)
{
    if (username.isEmpty()) {
        return PaintDisc(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BORDER_WIDGET), {}, {}, size, dpr);
    }
    // A stable hash (Qt's qHash is seeded per process) of the normalized
    // label, so look-alike spellings of one username share a colour.
    uint hash{0};
    for (const QChar ch : QString::fromStdString(platform::helpers::NormalizeLabel(username.toStdString()))) {
        hash = hash * 31 + ch.unicode();
    }
    const auto palette{avatarPalette()};
    const QColor fill{palette[hash % palette.size()]};
    const QColor glyph_color{fill.lightnessF() > 0.6 ? GUIUtil::getThemedQColor(GUIUtil::ThemedColor::DEFAULT)
                                                     : QColor{Qt::white}};
    const QString source{display_name.isEmpty() ? username : display_name};
    // The first character, whole when it lies outside the BMP.
    const int glyph_length{source.size() > 1 && source.at(0).isHighSurrogate() ? 2 : 1};
    return PaintDisc(fill, source.left(glyph_length).toUpper(), glyph_color, size, dpr);
}

QPixmap stepBadgePixmap(int number, int size, qreal dpr)
{
    return PaintDisc(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BLUE), QString::number(number), Qt::white, size, dpr);
}

QStringList registrationSteps(const platform::IdentityRecord& record)
{
    QStringList steps;
    if (record.funding_amount > 0) {
        steps << QObject::tr("Confirming your funding payment") << QObject::tr("Creating your identity");
    }
    steps << QObject::tr("Reserving your username") << QObject::tr("Registering your username");
    return steps;
}

int registrationStep(const platform::IdentityRecord& record)
{
    using State = platform::IdentityRecord::State;
    const int offset{record.funding_amount > 0 ? 2 : 0};
    const State state{record.state == State::NEEDS_UNLOCK || record.state == State::FAILED ? record.resume_state
                                                                                           : record.state};
    switch (state) {
    case State::FUNDING_SENT:
        return 0;
    case State::FUNDING_LOCKED:
    case State::IDENTITY_BROADCAST:
        return 1;
    case State::IDENTITY_CONFIRMED:
    case State::PREORDER_BROADCAST:
        return offset;
    case State::PREORDER_WAIT:
    case State::DOMAIN_BROADCAST:
        return offset + 1;
    case State::REGISTERED:
    case State::CONTESTED_PENDING:
        return offset + 2;
    case State::NONE:
    case State::NEEDS_UNLOCK:
    case State::FAILED:
        break;
    }
    return -1;
}

QString registrationStepLine(const platform::IdentityRecord& record)
{
    const QStringList steps{registrationSteps(record)};
    const int step{registrationStep(record)};
    if (step < 0 || step >= steps.size()) return {};
    return QObject::tr("Step %1 of %2: %3").arg(step + 1).arg(steps.size()).arg(steps.at(step));
}

QString formatPlatformBalance(BitcoinUnit unit, uint64_t credits)
{
    return BitcoinUnits::formatWithUnit(unit, static_cast<CAmount>(credits / platform::helpers::CreditsPerDuff()));
}

QString failureReassurance(const platform::IdentityRecord& record)
{
    using State = platform::IdentityRecord::State;
    switch (record.resume_state) {
    case State::IDENTITY_CONFIRMED:
    case State::PREORDER_BROADCAST:
    case State::PREORDER_WAIT:
    case State::DOMAIN_BROADCAST:
    case State::CONTESTED_PENDING:
    case State::REGISTERED:
        return QObject::tr("Your identity was created and keeps its balance on Dash Platform. Try again to register "
                           "a username; no new payment is needed.");
    case State::FUNDING_LOCKED:
    case State::IDENTITY_BROADCAST:
        return QObject::tr("Your funding payment is safe. Trying again reuses it; no new payment is made.");
    case State::FUNDING_SENT:
        // The payment never confirmed: its coins stayed in this wallet or
        // went where the payment that spent them sent them.
        return QObject::tr("Nothing was paid to Dash Platform. Try again to start a new registration.");
    case State::NONE:
    case State::NEEDS_UNLOCK:
    case State::FAILED:
        break;
    }
    return QObject::tr("No funds were spent.");
}

MessageLine::MessageLine(QWidget* parent) :
    QWidget(parent)
{
    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(TITLE_SPACING);

    auto* row{new QHBoxLayout()};
    row->setSpacing(TITLE_SPACING);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(MESSAGE_ICON_SIZE, MESSAGE_ICON_SIZE);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    // Messages name people by what they chose to be called: never markup.
    m_text->setTextFormat(Qt::PlainText);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_action = new QPushButton(this);
    makeSecondary(m_action);
    m_action->hide();
    m_toggle = new QPushButton(tr("Show details"), this);
    makeSecondary(m_toggle);
    m_toggle->hide();
    row->addWidget(m_icon, 0, Qt::AlignTop);
    row->addWidget(m_text, /*stretch=*/1);
    row->addWidget(m_action, 0, Qt::AlignTop);
    row->addWidget(m_toggle, 0, Qt::AlignTop);
    layout->addLayout(row);

    m_details = new QWidget(this);
    auto* details_layout{new QVBoxLayout(m_details)};
    details_layout->setContentsMargins(0, 0, 0, 0);
    details_layout->setSpacing(TITLE_SPACING);
    m_details_text = new QPlainTextEdit(m_details);
    m_details_text->setReadOnly(true);
    m_details_text->setFont(GUIUtil::fixedPitchFont());
    m_details_text->setMaximumHeight(m_details_text->fontMetrics().lineSpacing() * DETAILS_MAX_LINES +
                                     2 * m_details_text->frameWidth() + 8);
    auto* copy{new QPushButton(tr("Copy details"), m_details)};
    makeSecondary(copy);
    details_layout->addWidget(m_details_text);
    details_layout->addWidget(copy, 0, Qt::AlignRight);
    m_details->hide();
    layout->addWidget(m_details);

    connect(m_action, &QPushButton::clicked, this, &MessageLine::actionClicked);
    connect(m_toggle, &QPushButton::clicked, this, [this] {
        const bool show{!m_details->isVisible()};
        m_details->setVisible(show);
        m_toggle->setText(show ? tr("Hide details") : tr("Show details"));
    });
    connect(copy, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_details_text->toPlainText()); });
    hide();
}

void MessageLine::setMessage(Severity severity, const QString& text, const QString& details)
{
    if (text.isEmpty()) {
        clear();
        return;
    }
    m_severity = severity;
    m_text->setText(text);
    m_details_text->setPlainText(details);
    m_toggle->setVisible(!details.isEmpty());
    if (details.isEmpty() || !m_details->isVisible()) {
        m_details->hide();
        m_toggle->setText(tr("Show details"));
    }
    applySeverity();
    show();
}

void MessageLine::setTransientMessage(Severity severity, const QString& text)
{
    setMessage(severity, text);
    setAction({});
    QTimer::singleShot(TRANSIENT_MESSAGE_MS, this, [this, text] {
        if (m_text->text() == text) clear();
    });
}

void MessageLine::setAction(const QString& label)
{
    m_action->setText(label);
    m_action->setVisible(!label.isEmpty());
}

void MessageLine::setIconShown(bool shown)
{
    m_icon_shown = shown;
    applySeverity();
}

void MessageLine::clear()
{
    m_text->clear();
    m_details_text->clear();
    m_details->hide();
    m_toggle->hide();
    m_action->hide();
    hide();
}

QString MessageLine::text() const { return m_text->text(); }

void MessageLine::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange) applySeverity();
}

void MessageLine::applySeverity()
{
    QString icon;
    GUIUtil::ThemedColor color{GUIUtil::ThemedColor::DEFAULT};
    switch (m_severity) {
    case Severity::Info:
        setTextStyle(m_text, GUIUtil::ThemedStyle::TS_SECONDARY);
        break;
    case Severity::Success:
        icon = QStringLiteral("synced");
        color = GUIUtil::ThemedColor::GREEN;
        setTextStyle(m_text, GUIUtil::ThemedStyle::TS_SUCCESS);
        break;
    case Severity::Attention:
        icon = QStringLiteral("warning");
        color = GUIUtil::ThemedColor::ORANGE;
        setTextColor(m_text, color);
        break;
    case Severity::Error:
        icon = QStringLiteral("warning");
        color = GUIUtil::ThemedColor::RED;
        setTextStyle(m_text, GUIUtil::ThemedStyle::TS_ERROR);
        break;
    }
    const bool show_icon{m_icon_shown && !icon.isEmpty()};
    m_icon->setVisible(show_icon);
    if (show_icon) {
        m_icon->setPixmap(GUIUtil::getIcon(icon, color).pixmap(MESSAGE_ICON_SIZE, MESSAGE_ICON_SIZE));
    }
}
} // namespace PlatformUi
