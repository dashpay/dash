// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PROTOCOL_SER_PARAMS_H
#define BITCOIN_PROTOCOL_SER_PARAMS_H

#include <serialize.h>

/** Serialization parameters of the Dash objects whose encoding depends on where it goes: some fields are
 *  only stored on disk, and some P2P messages depend on the protocol version of the peer. */
struct ProtocolSerParams {
    enum class Format {
        Disk,    //!< Disk, hashes and RPC
        Network, //!< P2P message for a peer with protocol version nVersion
    };
    const Format fmt{Format::Disk};
    const int nVersion{0};

    static ProtocolSerParams Network(int version) { return {Format::Network, version}; }

    SER_PARAMS_OPFUNC
};

inline constexpr ProtocolSerParams PROTOCOL_DISK{ProtocolSerParams::Format::Disk};

#endif // BITCOIN_PROTOCOL_SER_PARAMS_H
