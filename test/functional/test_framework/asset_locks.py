#!/usr/bin/env python3
# Copyright (c) 2022-2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Shared setup and Asset Lock / Asset Unlock helpers for the feature_asset_locks*.py tests."""

import copy
import struct
from decimal import Decimal
from io import BytesIO

from test_framework.authproxy import JSONRPCException
from test_framework.messages import (
    CAssetLockTx,
    CAssetUnlockTx,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    tx_from_hex,
    hash256,
    ser_string,
)
from test_framework.script import (
    CScript,
    OP_RETURN,
)
from test_framework.script_util import (
    key_to_p2pk_script,
    key_to_p2pkh_script,
)
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

llmq_type_test = 106 # LLMQType::LLMQ_TEST_PLATFORM
MNEHF_SIGNAL_TX_TYPE = 7 # TRANSACTION_MNHF_SIGNAL
tiny_amount = int(Decimal("0.0007") * COIN)
blocks_in_one_day = 100
HEIGHT_DIFF_EXPIRING = 48


class AssetLocksMixin:
    """Mixed into the DashTestFramework subclasses of the feature_asset_locks*.py tests"""
    def set_asset_locks_test_params(self, extra_args=None):
        extra_args = extra_args or []
        self.set_dash_test_params(2, 0, [[
                "-whitelist=127.0.0.1",
                "-llmqtestinstantsenddip0024=llmq_test_instantsend",
                "-acceptnonstdtxn=1",
        ] + extra_args] * 2, evo_count=2)

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def create_assetlock(self, coin, amount, pubkey, version=1):
        node_wallet = self.nodes[0]


        coins = coin if isinstance(coin, list) else [coin]
        inputs = [CTxIn(COutPoint(int(c["txid"], 16), c["vout"])) for c in coins]

        credit_outputs = []
        tmp_amount = amount
        if tmp_amount > COIN:
            tmp_amount -= COIN
            credit_outputs.append(CTxOut(COIN, key_to_p2pkh_script(pubkey)))
        credit_outputs.append(CTxOut(tmp_amount, key_to_p2pkh_script(pubkey)))

        lockTx_payload = CAssetLockTx(version, credit_outputs)

        remaining = sum(int(COIN * c['amount']) for c in coins) - tiny_amount - amount

        tx_output_ret = CTxOut(amount, CScript([OP_RETURN, b""]))
        tx_output = CTxOut(remaining, key_to_p2pk_script(pubkey))

        lock_tx = CTransaction()
        lock_tx.vin = inputs
        lock_tx.vout = [tx_output, tx_output_ret] if remaining > 0 else [tx_output_ret]
        lock_tx.nVersion = 3
        lock_tx.nType = 8 # asset lock type
        lock_tx.vExtraPayload = lockTx_payload.serialize()

        lock_tx = node_wallet.signrawtransactionwithwallet(lock_tx.serialize().hex())
        return tx_from_hex(lock_tx["hex"])

    def create_assetunlock_request_id(self, index):
        # request ID = sha256("plwdtx", index)
        request_id_buf = ser_string(b"plwdtx") + struct.pack("<Q", index)
        return hash256(request_id_buf)[::-1].hex()

    def create_assetunlock(self, index, withdrawal, pubkey=None, fee=tiny_amount, version=1, requested_height=None):
        node_wallet = self.nodes[0]
        mninfo = self.mninfo
        assert_greater_than(int(withdrawal), fee)
        tx_output = CTxOut(int(withdrawal) - fee, key_to_p2pk_script(pubkey))

        request_id = self.create_assetunlock_request_id(index)

        height = node_wallet.getblockcount() if requested_height is None else requested_height
        self.log.info(f"Creating asset unlock: index={index} {request_id}")
        quorumHash = mninfo[0].get_node(self).quorum("selectquorum", llmq_type_test, request_id)["quorumHash"]
        self.log.info(f"Used quorum hash: {quorumHash}")
        unlockTx_payload = CAssetUnlockTx(
            version = version,
            index = index,
            fee = fee,
            requestedHeight = height,
            quorumHash = int(quorumHash, 16),
            quorumSig = b'\00' * 96)

        unlock_tx = CTransaction()
        unlock_tx.vin = []
        unlock_tx.vout = [tx_output]
        unlock_tx.nVersion = 3
        unlock_tx.nType = 9 # asset unlock type
        unlock_tx.vExtraPayload = unlockTx_payload.serialize()

        unlock_tx.calc_sha256()
        msgHash = format(unlock_tx.sha256, '064x')

        recsig = self.get_recovered_sig(request_id, msgHash, llmq_type=llmq_type_test, use_platformsign=True)

        unlockTx_payload.quorumSig = bytearray.fromhex(recsig["sig"])
        unlock_tx.vExtraPayload = unlockTx_payload.serialize()
        return unlock_tx

    def get_v2_txid(self, unlock_tx):
        # The txid of a v2 asset unlock: the tx hashed with the requestedHeight, quorumHash and
        # quorumSig payload fields zeroed - the fields Platform changes when it re-signs - so
        # every re-signed instance of one withdrawal shares one txid. The python-side rehash()
        # of the full serialization is the instance hash instead.
        payload = CAssetUnlockTx()
        payload.deserialize(BytesIO(unlock_tx.vExtraPayload))
        payload.requestedHeight = 0
        payload.quorumHash = 0
        payload.quorumSig = b'\x00' * 96
        tx_copy = copy.deepcopy(unlock_tx)
        tx_copy.vExtraPayload = payload.serialize()
        tx_copy.rehash()
        return tx_copy.hash

    def check_mempool_size(self):
        # Masternodes submit the MnEHF signal transaction on their own as soon as a quorum
        # able to sign it exists, so it is not part of what this test puts in the mempool.
        self.sync_mempools()
        for node in self.nodes:
            own = [txid for txid in node.getrawmempool()
                   if node.getrawtransaction(txid, 1)['type'] != MNEHF_SIGNAL_TX_TYPE]
            assert_equal(len(own), self.mempool_size)

    def set_sporks(self):
        spork_enabled = 0
        spork_disabled = 4070908800

        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", spork_enabled)
        self.nodes[0].sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", spork_disabled)
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", spork_disabled)
        self.wait_for_sporks_same()

    def send_tx_simple(self, tx):
        return self.nodes[0].sendrawtransaction(hexstring=tx.serialize().hex(), maxfeerate=0)

    def send_tx(self, tx, expected_error = None, reason = None):
        try:
            self.log.info(f"Send tx with expected_error:'{expected_error}'...")
            tx_res = self.send_tx_simple(tx)
            if expected_error is None:
                self.sync_mempools()
                return tx_res

            # failure didn't happen, but expected:
            message = "Transaction should not be accepted"
            if reason is not None:
                message += ": " + reason

            raise AssertionError(message)
        except JSONRPCException as e:
            assert expected_error in e.error['message']

    def generate_batch(self, count, sync_fun=None):
        self.log.info(f"Generate {count} blocks")
        while count > 0:
            self.log.info(f"Generating batch of blocks {count} left")
            batch = min(50, count)
            count -= batch
            self.bump_mocktime(10 * 60 + 1)
            self.generate(self.nodes[1], batch, sync_fun=sync_fun)

    # This functional test intentionally setup only 2 MN and only 2 Evo nodes
    # to ensure that corner case of quorum with minimum amount of nodes as possible
    # does not cause any issues in Dash Core
    def mine_quorum_2_nodes(self):
        self.mine_quorum(llmq_type_name='llmq_test_platform', expected_members=2, expected_connections=1, expected_contributions=2, expected_commitments=2, llmq_type=106)

    def create_asset_unlock_child(self, node_wallet, asset_unlock_tx, asset_unlock_txid):
        child_value = Decimal(asset_unlock_tx.vout[0].nValue - tiny_amount) / COIN
        child_hex = node_wallet.createrawtransaction(
            [{'txid': asset_unlock_txid, 'vout': 0}],
            {node_wallet.getnewaddress(): child_value})
        signed_child = node_wallet.signrawtransactionwithwallet(child_hex)
        assert signed_child['complete']
        child_txid = node_wallet.sendrawtransaction(signed_child['hex'])

        # Ensure package selection considers the child before its Asset Unlock
        # ancestor is considered on its own.
        node_wallet.prioritisetransaction(child_txid, COIN)
        return child_txid
