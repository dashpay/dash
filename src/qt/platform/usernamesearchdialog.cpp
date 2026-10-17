// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/usernamesearchdialog.h>

#include <platform/client.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

UsernameSearchDialog::UsernameSearchDialog(PlatformService& service, ContactFlow& flow, QWidget* parent) :
    QDialog(parent),
    m_service(service),
    m_flow(flow)
{
    setWindowTitle(tr("Search DashPay usernames"));
    m_input = new QLineEdit(this);
    m_input->setObjectName("dashPaySearchInput");
    m_input->setPlaceholderText(tr("Start of a username"));
    m_search = new QPushButton(tr("Search"), this);
    m_search->setObjectName("dashPaySearch");
    m_status = new QLabel(this);
    m_status->setObjectName("dashPaySearchStatus");
    m_status->setWordWrap(true);
    m_results = new QListWidget(this);
    m_results->setObjectName("dashPaySearchResults");
    m_add = new QPushButton(tr("Add contact"), this);
    m_add->setObjectName("dashPayAddFromSearch");
    // Enter searches in the input and does nothing elsewhere: it must not add
    // whatever an earlier search left chosen.
    m_search->setAutoDefault(false);
    m_add->setAutoDefault(false);

    auto* input_row{new QHBoxLayout};
    input_row->addWidget(m_input);
    input_row->addWidget(m_search);
    auto* layout{new QVBoxLayout(this)};
    layout->addLayout(input_row);
    layout->addWidget(m_status);
    layout->addWidget(m_results);
    layout->addWidget(m_add, 0, Qt::AlignRight);

    connect(m_search, &QPushButton::clicked, this, &UsernameSearchDialog::search);
    connect(m_input, &QLineEdit::returnPressed, this, &UsernameSearchDialog::search);
    connect(m_results, &QListWidget::itemSelectionChanged, this, &UsernameSearchDialog::updateAdd);
    connect(m_results, &QListWidget::itemDoubleClicked, this, &UsernameSearchDialog::add);
    connect(m_add, &QPushButton::clicked, this, &UsernameSearchDialog::add);
    connect(&m_service, &PlatformService::changed, this, &UsernameSearchDialog::update);
    connect(&m_flow, &ContactFlow::changed, this, &UsernameSearchDialog::updateAdd);
    m_status->hide();
    updateAdd();
}

void UsernameSearchDialog::search()
{
    const std::string prefix{m_input->text().trimmed().toStdString()};
    if (!m_prefix.empty() || prefix.size() < MIN_PREFIX_LENGTH) return;
    m_prefix = prefix;
    m_results->clear();
    m_status->setText(tr("Searching…"));
    m_status->show();
    run();
}

void UsernameSearchDialog::run()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return;
    client->searchNames(m_prefix, LIMIT, PlatformUi::Tracked<platform::Names>(m_call, [this](auto result) {
                            m_prefix.clear();
                            QString status{PlatformUi::Describe(result.status)};
                            if (result.ok()) {
                                for (const platform::DpnsName& name : result.value.names) {
                                    m_results->addItem(QString::fromStdString(name.label));
                                }
                                if (result.value.names.empty()) status = tr("No usernames found.");
                            }
                            m_status->setText(status);
                            m_status->setVisible(!status.isEmpty());
                        }));
}

void UsernameSearchDialog::update()
{
    // A call the client dropped never calls back.
    if (!m_prefix.empty() && m_call.expired()) run();
}

void UsernameSearchDialog::updateAdd()
{
    m_add->setEnabled(!m_results->selectedItems().isEmpty() && m_flow.step() == ContactFlow::Step::IDLE && m_flow.swept());
}

void UsernameSearchDialog::add()
{
    const auto selected{m_results->selectedItems()};
    if (selected.isEmpty() || !m_add->isEnabled()) return;
    m_flow.add(selected.front()->text().toStdString());
    accept();
}
