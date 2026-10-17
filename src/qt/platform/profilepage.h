// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PROFILEPAGE_H
#define BITCOIN_QT_PLATFORM_PROFILEPAGE_H

#include <platform/types.h>

#include <QWidget>

#include <optional>

class ProfileFlow;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
QT_END_NAMESPACE

/** The DashPay tab's profile: the identity's display name and public
 *  message. */
class ProfilePage : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilePage(QWidget* parent = nullptr);

    void setFlow(ProfileFlow* flow) { m_flow = flow; }
    //! Show the profile, and offer saving it only when `writable`: no
    //! DashPay flow, this one included, is busy.
    void refresh(bool writable);

private:
    ProfileFlow* m_flow{nullptr};
    QLineEdit* m_display_name{nullptr};
    QLineEdit* m_public_message{nullptr};
    QPushButton* m_save{nullptr};
    QLabel* m_status{nullptr};
    //! The profile the fields were filled from.
    std::optional<platform::Profile> m_shown;
};

#endif // BITCOIN_QT_PLATFORM_PROFILEPAGE_H
