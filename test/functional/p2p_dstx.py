#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test P2P CoinJoin broadcast transaction handling."""

import time

from test_framework.messages import (
    CCoinJoinBroadcastTx,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    msg_dstx,
)
from test_framework.p2p import P2PInterface
from test_framework.script import (
    CScript,
    OP_CHECKSIG,
    OP_DUP,
    OP_EQUALVERIFY,
    OP_HASH160,
)
from test_framework.test_framework import BitcoinTestFramework


class P2PDSTXTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-debug=net"]]

    def make_dstx(self):
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(hash=i + 1, n=0)) for i in range(3)]
        p2pkh = CScript([
            OP_DUP,
            OP_HASH160,
            b"\x01" * 20,
            OP_EQUALVERIFY,
            OP_CHECKSIG,
        ])
        tx.vout = [CTxOut(nValue=COIN // 1000 + 1, scriptPubKey=p2pkh) for _ in tx.vin]
        return CCoinJoinBroadcastTx(
            tx=tx,
            m_protxHash=1,
            vchSig=b"\x01" * 96,
            sigTime=int(time.time()),
        )

    def run_test(self):
        self.log.info("Leave IBD so unsolicited DSTX is processed")
        self.generate(self.nodes[0], 1)

        peer = self.nodes[0].add_p2p_connection(P2PInterface())

        self.log.info("Unknown masternode DSTX is treated as peer misbehavior")
        with self.nodes[0].assert_debug_log(["Misbehaving", "invalid dstx"]):
            peer.send_and_ping(msg_dstx(self.make_dstx()))


if __name__ == '__main__':
    P2PDSTXTest().main()
