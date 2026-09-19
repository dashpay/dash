// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PROTXSENDER_H
#define BITCOIN_QT_PROTXSENDER_H

#include <univalue.h>

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThread>

class WalletModel;

namespace interfaces {
class Node;
} // namespace interfaces

//! Outcome of a single RPC command executed by ProTxSender
struct ProTxResult
{
    bool ok{false};
    UniValue value;   //!< RPC result when ok
    int code{0};      //!< RPC error code otherwise
    QString message;  //!< user-displayable error otherwise
};

Q_DECLARE_METATYPE(ProTxResult)

//! The GUI's write path for masternode management: runs protx (and related) RPC
//! commands with named arguments on a dedicated worker thread against the wallet
//! selected by a WalletModel, and translates RPC errors into user-displayable
//! messages. Named-argument dispatch keeps the GUI independent of RPC parameter
//! order. Commands are serialized; one instance is meant to be owned per dialog.
class ProTxSender : public QObject
{
    Q_OBJECT

public:
    explicit ProTxSender(interfaces::Node& node, QObject* parent = nullptr);
    ~ProTxSender() override;

    //! True while a command is executing
    bool isBusy() const { return m_busy; }

    //! Execute `method` (the full command name, e.g. "protx register_fund") with
    //! named `params` against `wallet_model`'s wallet (nullptr for node-level
    //! commands). The outcome arrives via finished(). Returns false when a
    //! command is already in flight.
    bool execute(const QString& method, const UniValue& params, const WalletModel* wallet_model);

    //! Map an RPC error to a message a user can act on
    static QString translateError(int code, const QString& message);

Q_SIGNALS:
    void finished(const ProTxResult& result);

private:
    interfaces::Node& m_node;
    QThread m_thread;
    //! Plain context object living on m_thread that the calls are queued on
    QObject* const m_worker;
    bool m_busy{false};
};

#endif // BITCOIN_QT_PROTXSENDER_H
