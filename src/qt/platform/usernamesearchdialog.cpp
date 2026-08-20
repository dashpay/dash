// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/usernamesearchdialog.h>

#include <qt/guiutil.h>
#include <qt/platform/platformservice.h>
#include <util/strencodings.h>

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

UsernameSearchDialog::UsernameSearchDialog(PlatformService& service, QWidget* parent) :
    QDialog(parent),
    m_service(service)
{
    setWindowTitle(tr("Find people on DashPay"));
    resize(400, 420);
    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(tr("Search for someone by their DashPay username and send them a "
                               "contact request. Once they accept, you can pay each other by "
                               "name."),
                            this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Type a username, e.g. \"alice\""));
    layout->addWidget(m_input);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list);

    auto* buttons = new QDialogButtonBox(this);
    m_add = buttons->addButton(tr("Send contact request"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->insertWidget(layout->count() - 1, m_status);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(400);

    connect(m_input, &QLineEdit::textChanged, this, &UsernameSearchDialog::onTextChanged);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        const QString t = m_input->text().trimmed();
        if (!t.isEmpty()) m_service.searchNames(t);
    });
    connect(&m_service, &PlatformService::searchResults, this, &UsernameSearchDialog::onResults);
    connect(m_list, &QListWidget::currentItemChanged, this, &UsernameSearchDialog::updateActions);
    connect(m_add, &QPushButton::clicked, this, [this] { addSelected(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(&m_service, &PlatformService::contactRequestFinished, this, &UsernameSearchDialog::onContactRequestFinished);

    updateActions();
}

void UsernameSearchDialog::onTextChanged()
{
    m_debounce->stop();
    m_list->clear();
    m_status->setStyleSheet({});
    m_status->clear();
    m_searching = !m_input->text().trimmed().isEmpty();
    if (m_searching) {
        m_status->setText(tr("Searching…"));
        m_debounce->start();
    }
    updateActions();
}

void UsernameSearchDialog::onResults(const QString& prefix, const QVector<QPair<QString, QString>>& results)
{
    if (prefix.trimmed() != m_input->text().trimmed()) return;
    m_searching = false;
    m_list->clear();
    const auto my_id{m_service.myIdentityId()};
    const QString my_id_hex{my_id ? QString::fromStdString(HexStr(*my_id)) : QString{}};
    for (const auto& r : results) {
        QString text{r.first};                             // username
        const QString display_name{m_service.contactMetadata(r.second, "display-name")};
        if (!display_name.isEmpty()) text += QStringLiteral(" — ") + display_name;
        bool selectable{true};
        if (r.second == my_id_hex) {
            text += QStringLiteral(" ") + tr("(this is you)");
            selectable = false;
        } else if (m_service.readRecord("contact/key/" + r.second.toStdString()).size() == 65) {
            text += QStringLiteral(" ") + tr("(already a contact)");
            selectable = false;
        } else if (!m_service.readRecord("contact/out/" + r.second.toStdString()).empty()) {
            text += QStringLiteral(" ") + tr("(request already sent)");
            selectable = false;
        }
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, r.second);             // identity hex
        if (!selectable) item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled);
    }
    m_status->setStyleSheet({});
    m_status->setText(results.isEmpty() ? tr("No usernames found.") : QString{});
    updateActions();
}

void UsernameSearchDialog::updateActions()
{
    const auto* item{m_list->currentItem()};
    const bool valid_selection{item && !item->data(Qt::UserRole).toString().isEmpty()};
    m_add->setEnabled(valid_selection && !m_searching && m_pending_identity.isEmpty());
}

void UsernameSearchDialog::addSelected()
{
    auto* item = m_list->currentItem();
    if (!item) return;
    m_pending_identity = item->data(Qt::UserRole).toString();
    m_input->setEnabled(false);
    m_list->setEnabled(false);
    updateActions();
    m_status->setText(tr("Sending and verifying contact request…"));
    QString error;
    if (!m_service.sendContactRequest(m_pending_identity, error)) {
        onContactRequestFinished(m_pending_identity, false, error);
    }
}

void UsernameSearchDialog::onContactRequestFinished(const QString& id, bool ok, const QString& error)
{
    if (m_pending_identity != id) return;
    m_pending_identity.clear();
    m_input->setEnabled(true);
    m_list->setEnabled(true);
    if (ok) {
        for (int i = 0; i < m_list->count(); ++i) {
            auto* item{m_list->item(i)};
            if (item->data(Qt::UserRole).toString() != id) continue;
            item->setText(item->text() + QStringLiteral(" ") + tr("(request already sent)"));
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled);
            m_list->clearSelection();
            m_list->setCurrentItem(nullptr);
            break;
        }
        m_status->setStyleSheet(
            QString{"QLabel { color: %1; }"}.arg(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::GREEN).name()));
        m_status->setText(tr("Contact request sent to %1. You will be connected as soon as "
                             "they accept it.")
                              .arg(m_service.contactDisplayString(id)));
    } else {
        m_status->setStyleSheet(
            QString{"QLabel { color: %1; }"}.arg(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::RED).name()));
        m_status->setText(tr("Could not send the request: %1").arg(error));
    }
    updateActions();
}
