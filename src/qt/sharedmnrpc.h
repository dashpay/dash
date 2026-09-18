// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SHAREDMNRPC_H
#define BITCOIN_QT_SHAREDMNRPC_H

//! The shared masternode RPC method names the GUI dispatches through
//! interfaces::Node::executeRpc. A name that is not registered only fails at
//! call time, deep inside a multi-party flow, so they live in one place and
//! MnShareSessionTests::rpcMethodNamesAreRegistered() checks every one of them
//! against the command table in src/rpc/evo.cpp.
namespace shared_mn_rpc {
inline constexpr char REGISTER_PREPARE[]{"protx shared_register_prepare"};
inline constexpr char SIGN[]{"protx shared_sign"};
inline constexpr char COMBINE[]{"protx shared_combine"};
inline constexpr char DISSOLVE[]{"protx shared_dissolve"};
inline constexpr char DISSOLVE_PREPARE[]{"protx shared_dissolve_prepare"};
inline constexpr char UPDATE_SHARE[]{"protx shared_update_share"};
inline constexpr char UPDATE_REGISTRAR_PREPARE[]{"protx shared_update_registrar_prepare"};

//! Every name above, in one array for the regression test
inline constexpr const char* ALL[]{
    REGISTER_PREPARE, SIGN, COMBINE, DISSOLVE, DISSOLVE_PREPARE, UPDATE_SHARE, UPDATE_REGISTRAR_PREPARE,
};
} // namespace shared_mn_rpc

#endif // BITCOIN_QT_SHAREDMNRPC_H
