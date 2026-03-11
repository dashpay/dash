#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import os
import sys
from typing import List

# Allow imports from the functional test framework.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'functional'))

from test_framework.authproxy import JSONRPCException  # noqa: E402
from test_framework.test_node import TestNode  # noqa: E402
from test_framework.wallet import MiniWallet  # noqa: E402


def create_self_transfer_batch(
    wallet: MiniWallet,
    count: int,
) -> List[str]:
    """Create *count* signed self-transfer transactions from *wallet* UTXOs."""
    txs: List[str] = []
    for _ in range(count):
        tx = wallet.create_self_transfer()
        txs.append(tx["hex"])
    return txs


def submit_transactions(
    node: TestNode,
    tx_hexes: List[str],
) -> List[str]:
    """Submit a list of raw transactions to *node* via ``sendrawtransaction``."""
    txids: List[str] = []
    for tx_hex in tx_hexes:
        try:
            txid = node.sendrawtransaction(tx_hex)
            txids.append(txid)
        except JSONRPCException:
            pass
    return txids
