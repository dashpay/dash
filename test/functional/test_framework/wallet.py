#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A limited-functionality wallet, which may replace a real wallet in tests"""

from copy import deepcopy
from decimal import Decimal
from test_framework.address import ADDRESS_BCRT1_P2SH_OP_TRUE
from random import choice
from typing import Optional
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    tx_from_hex,
)
from test_framework.script import (
    CScript,
    OP_TRUE,
)
from test_framework.util import (
    assert_equal,
    hex_str_to_bytes,
    assert_greater_than_or_equal,
    satoshi_round,
)

DEFAULT_FEE = Decimal("0.0001")

class MiniWallet:
    def __init__(self, test_node):
        self._test_node = test_node
        self._utxos = []
        self._address = ADDRESS_BCRT1_P2SH_OP_TRUE
        self._scriptPubKey = hex_str_to_bytes(self._test_node.validateaddress(self._address)['scriptPubKey'])

    def scan_blocks(self, *, start=1, num):
        """Scan the blocks for self._address outputs and add them to self._utxos"""
        for i in range(start, start + num):
            block = self._test_node.getblock(blockhash=self._test_node.getblockhash(i), verbosity=2)
            for tx in block['tx']:
                for out in tx['vout']:
                    if out['scriptPubKey']['hex'] == self._scriptPubKey.hex():
                        self._utxos.append({'txid': tx['txid'], 'vout': out['n'], 'value': out['value']})

    def generate(self, num_blocks):
        """Generate blocks with coinbase outputs to the internal address, and append the outputs to the internal list"""
        blocks = self._test_node.generatetoaddress(num_blocks, self._address)
        for b in blocks:
            cb_tx = self._test_node.getblock(blockhash=b, verbosity=2)['tx'][0]
            self._utxos.append({'txid': cb_tx['txid'], 'vout': 0, 'value': cb_tx['vout'][0]['value']})
        return blocks

    def get_address(self):
        return self._address

    def get_utxo(self, *, txid: Optional[str]=''):
        """
        Returns a utxo and marks it as spent (pops it from the internal list)

        Args:
        txid: get the first utxo we find from a specific transaction

        Note: Can be used to get the change output immediately after a send_self_transfer
        """
        index = -1  # by default the last utxo
        if txid:
            utxo = next(filter(lambda utxo: txid == utxo['txid'], self._utxos))
            index = self._utxos.index(utxo)
        return self._utxos.pop(index)

    def send_self_transfer(self, *, fee_rate=Decimal("0.003"), from_node, utxo_to_spend=None):
        """Create and send a tx with the specified fee_rate. Fee may be exact or at most one satoshi higher than needed."""
        self._utxos = sorted(self._utxos, key=lambda k: k['value'])
        utxo_to_spend = utxo_to_spend or self._utxos.pop()  # Pick the largest utxo (if none provided) and hope it covers the fee
        vsize = Decimal(85)
        send_value = satoshi_round(utxo_to_spend['value'] - fee_rate * (vsize / 1000))
        fee = utxo_to_spend['value'] - send_value
        assert send_value > 0

        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(utxo_to_spend['txid'], 16), utxo_to_spend['vout']))]
        tx.vout = [CTxOut(int(send_value * COIN), self._scriptPubKey)]
        tx.vin[0].scriptSig = CScript([CScript([OP_TRUE])])
        tx_hex = tx.serialize().hex()

        tx_info = from_node.testmempoolaccept([tx_hex])[0]
        self._utxos.append({'txid': tx_info['txid'], 'vout': 0, 'value': send_value})
        from_node.sendrawtransaction(tx_hex)
        assert_equal(len(tx_hex) // 2, vsize)
        assert_equal(tx_info['fees']['base'], fee)
        return {'txid': tx_info['txid'], 'hex': tx_hex}
        self.scan_tx(from_node.decoderawtransaction(tx_hex))

def make_chain(node, address, privkeys, parent_txid, parent_value, n=0, parent_locking_script=None, fee=DEFAULT_FEE):
    """Build a transaction that spends parent_txid.vout[n] and produces one output with
    amount = parent_value with a fee deducted.
    Return tuple (CTransaction object, raw hex, nValue, scriptPubKey of the output created).
    """
    inputs = [{"txid": parent_txid, "vout": n}]
    my_value = parent_value - fee
    outputs = {address : my_value}
    rawtx = node.createrawtransaction(inputs, outputs)
    prevtxs = [{
        "txid": parent_txid,
        "vout": n,
        "scriptPubKey": parent_locking_script,
        "amount": parent_value,
    }] if parent_locking_script else None
    signedtx = node.signrawtransactionwithkey(hexstring=rawtx, privkeys=privkeys, prevtxs=prevtxs)
    assert signedtx["complete"]
    tx = tx_from_hex(signedtx["hex"])
    return (tx, signedtx["hex"], my_value, tx.vout[0].scriptPubKey.hex())

def create_child_with_parents(node, address, privkeys, parents_tx, values, locking_scripts, fee=DEFAULT_FEE):
    """Creates a transaction that spends the first output of each parent in parents_tx."""
    num_parents = len(parents_tx)
    total_value = sum(values)
    inputs = [{"txid": tx.rehash(), "vout": 0} for tx in parents_tx]
    outputs = {address : total_value - fee}
    rawtx_child = node.createrawtransaction(inputs, outputs)
    prevtxs = []
    for i in range(num_parents):
        prevtxs.append({"txid": parents_tx[i].rehash(), "vout": 0, "scriptPubKey": locking_scripts[i], "amount": values[i]})
    signedtx_child = node.signrawtransactionwithkey(hexstring=rawtx_child, privkeys=privkeys, prevtxs=prevtxs)
    assert signedtx_child["complete"]
    return signedtx_child["hex"]

def create_raw_chain(node, first_coin, address, privkeys, chain_length=25):
    """Helper function: create a "chain" of chain_length transactions. The nth transaction in the
    chain is a child of the n-1th transaction and parent of the n+1th transaction.
    """
    parent_locking_script = None
    txid = first_coin["txid"]
    chain_hex = []
    chain_txns = []
    value = first_coin["amount"]

    for _ in range(chain_length):
        (tx, txhex, value, parent_locking_script) = make_chain(node, address, privkeys, txid, value, 0, parent_locking_script)
        txid = tx.rehash()
        chain_hex.append(txhex)
        chain_txns.append(tx)

    return (chain_hex, chain_txns)

def bulk_transaction(tx, node, target_weight, privkeys, prevtxs=None):
    """Pad a transaction with extra outputs until it reaches a target weight (or higher).
    returns CTransaction object
    """
    tx_heavy = deepcopy(tx)
    assert_greater_than_or_equal(target_weight, tx_heavy.get_weight())
    while tx_heavy.get_weight() < target_weight:
        random_spk = "6a4d0200"  # OP_RETURN OP_PUSH2 512 bytes
        for _ in range(512*2):
            random_spk += choice("0123456789ABCDEF")
        tx_heavy.vout.append(CTxOut(0, bytes.fromhex(random_spk)))
    # Re-sign the transaction
    if privkeys:
        signed = node.signrawtransactionwithkey(tx_heavy.serialize().hex(), privkeys, prevtxs)
        return tx_from_hex(signed["hex"])
    return tx_heavy
