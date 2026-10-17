// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/profilepage.h>

#include <qt/guiutil.h>
#include <qt/platform/profileflow.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

ProfilePage::ProfilePage(QWidget* parent) :
    QWidget(parent)
{
    auto* title{new QLabel(tr("Your profile"), this)};
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold);
    m_display_name = new QLineEdit(this);
    m_display_name->setObjectName("dashPayProfileName");
    m_display_name->setPlaceholderText(tr("Display name"));
    m_public_message = new QLineEdit(this);
    m_public_message->setObjectName("dashPayProfileMessage");
    m_public_message->setPlaceholderText(tr("Public message"));
    m_save = new QPushButton(tr("Save profile"), this);
    m_save->setObjectName("dashPaySaveProfile");
    m_status = new QLabel(this);
    m_status->setObjectName("dashPayProfileStatus");
    m_status->setWordWrap(true);

    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(title);
    layout->addWidget(m_display_name);
    layout->addWidget(m_public_message);
    layout->addWidget(m_save, 0, Qt::AlignLeft);
    layout->addWidget(m_status);

    connect(m_save, &QPushButton::clicked, this,
            [this] { m_flow->save(m_display_name->text().trimmed(), m_public_message->text().trimmed()); });
}

void ProfilePage::refresh(bool writable)
{
    if (!m_flow) return;
    using Step = ProfileFlow::Step;
    // The fields show what Platform holds once it changes, which is also
    // after a save.
    const auto shown{m_flow->own()};
    if (m_flow->ownRead() && shown != m_shown) {
        m_shown = shown;
        m_display_name->setText(shown ? QString::fromStdString(shown->display_name) : QString{});
        m_public_message->setText(shown ? QString::fromStdString(shown->public_message) : QString{});
    }

    QString status;
    switch (m_flow->step()) {
    case Step::IDLE:
        status = m_flow->error();
        break;
    case Step::SAVING:
        status = tr("Publishing your profile…");
        break;
    }
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    m_save->setVisible(writable && m_flow->ownRead());
}
