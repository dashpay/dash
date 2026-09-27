// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/profiledialog.h>

#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/sharedmnwidgets.h>
#include <util/strencodings.h>

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

using Severity = PlatformUi::MessageLine::Severity;

namespace {
constexpr int MIN_WIDTH{440};
constexpr int MESSAGE_LINES{3};
} // namespace

ProfileDialog::ProfileDialog(PlatformService& service, QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    m_service(service)
{
    setWindowTitle(tr("Edit DashPay profile"));
    setMinimumWidth(MIN_WIDTH);
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(MasternodeWidgetUtil::GROUP_SPACING);

    layout->addWidget(PlatformUi::makeHint(tr("Your profile is public. Anyone on Dash Platform can see it; it helps "
                                              "contacts recognize you."),
                                           this));

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    // Each label on the line of its field, the counters beside them.
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_display_name = new QLineEdit(this);
    m_display_name->setMaxLength(PlatformService::MAX_DISPLAY_NAME_LENGTH);
    m_display_name->setPlaceholderText(tr("e.g. your name or nickname"));
    // A counter keeps to its text: a label that could grow would make its
    // row take the dialog's spare height and push the field off its label.
    const auto make_counter = [this] {
        auto* counter{new QLabel(this)};
        counter->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        counter->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        PlatformUi::setTextStyle(counter, GUIUtil::ThemedStyle::TS_SECONDARY);
        return counter;
    };
    m_name_counter = make_counter();
    auto* name_row = new QHBoxLayout();
    name_row->addWidget(m_display_name, /*stretch=*/1);
    name_row->addWidget(m_name_counter);
    form->addRow(tr("Display name"), name_row);
    m_public_message = new QPlainTextEdit(this);
    m_public_message->setTabChangesFocus(true);
    m_public_message->setFixedHeight(m_public_message->fontMetrics().lineSpacing() * MESSAGE_LINES +
                                     2 * m_public_message->frameWidth() + 8);
    m_message_counter = make_counter();
    auto* message_row = new QHBoxLayout();
    message_row->addWidget(m_public_message, /*stretch=*/1);
    message_row->addWidget(m_message_counter, 0, Qt::AlignTop);
    form->addRow(tr("Public message"), message_row);
    // The label of the multi-line field sits by its first line.
    if (QLayoutItem * label{form->itemAt(1, QFormLayout::LabelRole)}) label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addLayout(form);

    m_busy = PlatformUi::makeBusyBar(this);
    layout->addWidget(m_busy);
    m_status = new PlatformUi::MessageLine(this);
    layout->addWidget(m_status);
    // Spare height goes below the form, never between its rows.
    layout->addStretch();

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    PlatformUi::makeSecondary(m_buttons->button(QDialogButtonBox::Cancel));
    m_buttons->button(QDialogButtonBox::Save)->setDefault(true);
    layout->addWidget(m_buttons);
    setTabOrder(m_display_name, m_public_message);
    setTabOrder(m_public_message, m_buttons->button(QDialogButtonBox::Save));
    setTabOrder(m_buttons->button(QDialogButtonBox::Save), m_buttons->button(QDialogButtonBox::Cancel));

    connect(m_display_name, &QLineEdit::textChanged, this, &ProfileDialog::updateState);
    connect(m_public_message, &QPlainTextEdit::textChanged, this, &ProfileDialog::updateState);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &ProfileDialog::save);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &ProfileDialog::reject);
    connect(m_status, &PlatformUi::MessageLine::actionClicked, this, &ProfileDialog::load);
    connect(&m_service, &PlatformService::profileUpdated, this, &ProfileDialog::onProfileUpdated);
    connect(&m_service, &PlatformService::profileLoaded, this,
            [this](const QString& identity_hex, const QString& display, const QString& message) {
                const auto my_id{m_service.myIdentityId()};
                if (m_loaded || !my_id || QString::fromStdString(HexStr(*my_id)) != identity_hex) return;
                m_loaded = true;
                m_loaded_name = display;
                m_loaded_message = message;
                m_display_name->setText(display);
                m_public_message->setPlainText(message);
                if (display.isEmpty() && message.isEmpty()) setWindowTitle(tr("Create DashPay profile"));
                m_busy->hide();
                m_status->clear();
                updateState();
                m_display_name->setFocus();
            });
    connect(&m_service, &PlatformService::profileLoadFailed, this,
            [this](const QString& identity_hex, const QString& error, const QString& details) {
                const auto my_id{m_service.myIdentityId()};
                if (m_loaded || !my_id || QString::fromStdString(HexStr(*my_id)) != identity_hex) return;
                m_busy->hide();
                m_status->setMessage(Severity::Error,
                                     tr("Your current profile could not be loaded, so it can't be edited safely right "
                                        "now. %1")
                                         .arg(error),
                                     details);
                m_status->setAction(tr("Retry"));
                adjustSize();
            });

    GUIUtil::updateFonts();
    GUIUtil::disableMacFocusRect(this);
    SharedMnFitWrappedLabels(this);
    load();
}

void ProfileDialog::load()
{
    m_busy->show();
    m_status->setMessage(Severity::Info, tr("Loading your current profile…"));
    m_status->setAction({});
    updateState();
    if (const auto id{m_service.myIdentityId()}) m_service.loadProfile(*id);
}

void ProfileDialog::updateState()
{
    const int name_length{static_cast<int>(m_display_name->text().size())};
    const int message_length{static_cast<int>(m_public_message->toPlainText().size())};
    m_name_counter->setText(QStringLiteral("%1/%2").arg(name_length).arg(PlatformService::MAX_DISPLAY_NAME_LENGTH));
    m_message_counter->setText(QStringLiteral("%1/%2").arg(message_length).arg(PlatformService::MAX_PUBLIC_MESSAGE_LENGTH));
    // Both counters as wide as the longest count either shows, so the two
    // fields end at the same edge and keep it while typing.
    const QString widest{QStringLiteral("%1/%1").arg(PlatformService::MAX_PUBLIC_MESSAGE_LENGTH)};
    const int counter_width{GUIUtil::TextWidth(m_message_counter->fontMetrics(), widest) + 2 * m_message_counter->margin()};
    m_name_counter->setFixedWidth(counter_width);
    m_message_counter->setFixedWidth(counter_width);
    const bool over{message_length > PlatformService::MAX_PUBLIC_MESSAGE_LENGTH};
    if (over) {
        PlatformUi::setTextStyle(m_message_counter, GUIUtil::ThemedStyle::TS_ERROR);
    } else {
        PlatformUi::setTextStyle(m_message_counter, GUIUtil::ThemedStyle::TS_SECONDARY);
    }
    const bool editable{m_loaded && !m_pending};
    m_display_name->setEnabled(editable);
    m_public_message->setEnabled(editable);
    const bool changed{m_display_name->text().trimmed() != m_loaded_name ||
                       m_public_message->toPlainText() != m_loaded_message};
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(editable && changed && !over);
    m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!m_pending);
}

void ProfileDialog::save()
{
    if (!m_loaded || m_pending) return;
    m_pending = true;
    m_busy->show();
    m_status->setMessage(Severity::Info, tr("Saving your profile to Dash Platform… This can take up to 30 seconds."));
    updateState();
    QString error;
    if (!m_service.updateProfile(m_display_name->text().trimmed(), m_public_message->toPlainText(), error)) {
        onProfileUpdated(false, error, {});
    }
}

void ProfileDialog::onProfileUpdated(bool ok, const QString& error, const QString& details)
{
    if (!m_pending) return;
    m_pending = false;
    m_busy->hide();
    if (ok) {
        accept();
        return;
    }
    m_status->setMessage(Severity::Error, tr("Your profile was not saved. %1").arg(error), details);
    updateState();
    // Tall enough for the whole message.
    adjustSize();
}

void ProfileDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::StyleChange) PlatformUi::restyle(this);
}

void ProfileDialog::closeEvent(QCloseEvent* event)
{
    if (m_pending) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void ProfileDialog::reject()
{
    if (!m_pending) QDialog::reject();
}
