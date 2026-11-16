#!/usr/bin/env python3

# Copyright (c) 2022-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Test Asset Lock and Asset Unlock transactions through the v24 fork.

Version 2 asset unlocks, non-standard asset locks and crash replay are in
feature_asset_locks_v24.py.
"""

import copy
from decimal import Decimal
from io import BytesIO

from test_framework.asset_locks import (
    AssetLocksMixin,
    HEIGHT_DIFF_EXPIRING,
    blocks_in_one_day,
    llmq_type_test,
    tiny_amount,
)
from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.authproxy import JSONRPCException
from test_framework.messages import (
    CAssetUnlockTx,
    COIN,
    msg_tx,
    tx_from_hex,
)
from test_framework.p2p import P2PInterface
from test_framework.script import hash160
from test_framework.script_util import key_to_p2pkh_script
from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
    softfork_active,
)
from test_framework.segwit_addr import encode_platform_p2pkh
from test_framework.wallet_util import generate_keypair


class AssetLocksTest(AssetLocksMixin, DashTestFramework):
    def set_test_params(self):
        self.set_asset_locks_test_params()
        self.mn_rr_height = 560

    def create_assetunlock_for_oldest_quorum(self, start_index, withdrawal, pubkey):
        expected_quorum_hash = self.nodes[0].quorum('list')['llmq_test_platform'][-1]
        # Quorum selection depends on the request ID. Scan 100 candidates to
        # avoid missing the target oldest quorum by chance.
        for index in range(start_index, start_index + 100):
            request_id = self.create_assetunlock_request_id(index)
            quorum_hash = self.mninfo[0].get_node(self).quorum("selectquorum", llmq_type_test, request_id)["quorumHash"]
            if quorum_hash == expected_quorum_hash:
                self.log.info(f"Selected asset unlock index={index} for oldest active quorum {expected_quorum_hash}")
                asset_unlock_tx = self.create_assetunlock(index, withdrawal, pubkey)
                asset_unlock_tx_payload = CAssetUnlockTx()
                asset_unlock_tx_payload.deserialize(BytesIO(asset_unlock_tx.vExtraPayload))
                assert_equal(format(asset_unlock_tx_payload.quorumHash, '064x'), expected_quorum_hash)
                return asset_unlock_tx, asset_unlock_tx_payload, expected_quorum_hash

        raise AssertionError("Unable to select oldest active platform quorum")

    def get_credit_pool_balance(self, node = None, block_hash = None):
        if node is None:
            node = self.nodes[0]

        if block_hash is None:
            block_hash = node.getbestblockhash()
        block = node.getblock(block_hash)
        return int(COIN * block['cbTx']['creditPoolBalance'])

    def validate_credit_pool_balance(self, expected = None, block_hash = None):
        for node in self.nodes:
            locked = self.get_credit_pool_balance(node=node, block_hash=block_hash)
            if expected is None:
                expected = locked
            else:
                assert_equal(expected, locked)
        self.log.info(f"Credit pool amount matched with '{expected}'")
        return expected

    def check_mempool_result(self, result_expected, tx, txid=None):
        """Wrapper to check result of testmempoolaccept on node_0's mempool"""
        result_expected['txid'] = txid or tx.rehash()
        if result_expected['allowed']:
            result_expected['vsize'] = tx.get_vsize()

        result_test = self.nodes[0].testmempoolaccept([tx.serialize().hex()])
        for r in result_test:
            # Skip these checks for now
            if "fees" in r:
                r["fees"].pop("effective-feerate")
                r["fees"].pop("effective-includes")

        assert_equal([result_expected], result_test)
        self.check_mempool_size()

    def create_and_check_block(self, txes, expected_error = None):
        node_wallet = self.nodes[0]
        best_block_hash = node_wallet.getbestblockhash()
        best_block = node_wallet.getblock(best_block_hash)
        tip = int(best_block_hash, 16)
        height = best_block["height"] + 1
        block_time = best_block["time"] + 1

        cbb = create_coinbase(height, dip4_activated=True, v20_activated=True)
        gbt = node_wallet.getblocktemplate()
        cbb.vExtraPayload = bytes.fromhex(gbt["coinbase_payload"])
        cbb.rehash()
        block = create_block(tip, cbb, block_time, version=4)
        # Add quorum commitments from block template
        for tx_obj in gbt["transactions"]:
            tx = tx_from_hex(tx_obj["data"])
            if tx.nType == 6:
                block.vtx.append(tx)
        for tx in txes:
            block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()
        result = node_wallet.submitblock(block.serialize().hex())
        if result != expected_error:
            raise AssertionError('mining the block should have failed with error %s, but submitblock returned %s' % (expected_error, result))

    def ensure_tx_is_not_mined(self, tx_id):
        try:
            for node in self.nodes:
                node.gettransaction(tx_id)
            raise AssertionError("Transaction should not be mined")
        except JSONRPCException as e:
            assert "Invalid or non-wallet transaction id" in e.error['message']

    def run_test(self):
        node_wallet = self.nodes[0]
        node = self.nodes[1]

        self.set_sporks()

        assert_equal(self.nodes[0].getdeploymentinfo()['deployments']['v20']['active'], True)

        for _ in range(2):
            self.dynamically_add_masternode(evo=True)

        self.mempool_size = 0

        privkey, pubkey = generate_keypair(wif=True)
        node_wallet.importprivkey(privkey)

        self.test_asset_locks(node_wallet, node, pubkey)
        self.test_asset_unlocks(node_wallet, node, pubkey)
        self.test_withdrawal_limits(node_wallet, node, pubkey)
        self.test_mn_rr(node_wallet, node, pubkey)
        self.test_withdrawals_fork(node_wallet, node, pubkey)
        self.test_asset_locks_v2_pre_v24(node_wallet, node, pubkey)
        self.test_v24_fork(node_wallet, node, pubkey)

    def test_asset_locks(self, node_wallet, node, pubkey):
        self.log.info("Testing asset lock...")
        locked_1 = 10 * COIN + 141421
        locked_2 = 10 * COIN + 314159

        coins = node_wallet.listunspent(query_options={'minimumAmount': Decimal(str(locked_2 / COIN))})
        coin = coins.pop()
        asset_lock_tx = self.create_assetlock(coin, locked_1, pubkey)


        self.check_mempool_result(tx=asset_lock_tx, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})
        self.validate_credit_pool_balance(0)
        txid_in_block = self.send_tx(asset_lock_tx)
        rpc_tx = node.getrawtransaction(txid_in_block, 1)
        assert_equal(len(rpc_tx["assetLockTx"]["creditOutputs"]), 2)
        assert_equal(rpc_tx["assetLockTx"]["creditOutputs"][0]["valueSat"] + rpc_tx["assetLockTx"]["creditOutputs"][1]["valueSat"], locked_1)
        assert_equal(rpc_tx["assetLockTx"]["creditOutputs"][0]["scriptPubKey"]["hex"], key_to_p2pkh_script(pubkey).hex())
        assert_equal(rpc_tx["assetLockTx"]["creditOutputs"][1]["scriptPubKey"]["hex"], key_to_p2pkh_script(pubkey).hex())
        self.validate_credit_pool_balance(0)
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(self.get_credit_pool_balance(node=node), locked_1)
        self.log.info("Generate a number of blocks to ensure this is the longest chain for later in the test when we reconsiderblock")
        self.generate(node, 12)

        self.validate_credit_pool_balance(locked_1)

        # tx is mined, let's get blockhash
        self.log.info("Invalidate block with asset lock tx...")
        self.block_hash_1 = node_wallet.gettransaction(txid_in_block)['blockhash']
        for inode in self.nodes:
            inode.invalidateblock(self.block_hash_1)
            assert_equal(self.get_credit_pool_balance(node=inode), 0)
        self.generate(node, 3)
        self.validate_credit_pool_balance(0)
        self.log.info("Resubmit asset lock tx to new chain...")
        # NEW tx appears
        asset_lock_tx_2 = self.create_assetlock(coin, locked_2, pubkey)
        txid_in_block = self.send_tx(asset_lock_tx_2)
        self.generate(node, 1)
        self.validate_credit_pool_balance(locked_2)
        self.log.info("Reconsider old blocks...")
        for inode in self.nodes:
            inode.reconsiderblock(self.block_hash_1)
        self.validate_credit_pool_balance(locked_1)
        self.sync_all()

        self.log.info('Mine block with incorrect credit-pool value...')
        coin = coins.pop()
        extra_lock_tx = self.create_assetlock(coin, COIN, pubkey)
        self.create_and_check_block([extra_lock_tx], expected_error = 'bad-cbtx-assetlocked-amount')

        self.log.info("Mine IS quorum")
        if len(self.nodes[0].quorum('list')['llmq_test_instantsend']) == 0:
            self.mine_quorum(llmq_type_name='llmq_test_instantsend', expected_members=2, expected_connections=1, expected_contributions=2, expected_commitments=2, llmq_type=104)
        else:
            self.log.info("IS quorum exist")

        self.log.info("Mine a platform quorum")
        if len(self.nodes[0].quorum('list')['llmq_test_platform']) == 0:
            self.mine_quorum_2_nodes()
        else:
            self.log.info("Platform quorum exist")

        self.validate_credit_pool_balance(locked_1)

    def test_asset_unlocks(self, node_wallet, node, pubkey):
        self.log.info("Testing asset unlock...")

        self.log.info("Generating several txes by same quorum....")
        locked = self.get_credit_pool_balance()

        self.validate_credit_pool_balance(locked)
        asset_unlock_tx = self.create_assetunlock(101, COIN, pubkey)
        asset_unlock_tx_late = self.create_assetunlock(102, COIN, pubkey)
        asset_unlock_tx_too_late = self.create_assetunlock(103, COIN, pubkey)
        asset_unlock_tx_too_big_fee = self.create_assetunlock(104, COIN, pubkey, fee=int(Decimal("0.1") * COIN))
        asset_unlock_tx_zero_fee = self.create_assetunlock(105, COIN, pubkey, fee=0)
        asset_unlock_tx_duplicate_index = copy.deepcopy(asset_unlock_tx)
        # modify this tx with duplicated index to make a hash of tx different, otherwise tx would be refused too early
        asset_unlock_tx_duplicate_index.vout[0].nValue += COIN
        too_late_height = node.getblockcount() + HEIGHT_DIFF_EXPIRING

        self.log.info("Mine block to empty mempool")
        self.bump_mocktime(10 * 60 + 1)
        self.generate(self.nodes[0], 1)

        self.check_mempool_result(tx=asset_unlock_tx, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})
        self.check_mempool_result(tx=asset_unlock_tx_too_big_fee,
                result_expected={'allowed': False, 'reject-reason' : 'max-fee-exceeded'})
        self.check_mempool_result(tx=asset_unlock_tx_zero_fee,
                result_expected={'allowed': False, 'reject-reason' : 'bad-txns-assetunlock-fee-outofrange'})
        # not-verified is a correct error message when adding to mempool because Mempool knows nothing about CreditPool and indexes.
        # but the signature is invalid so far as we changed index without re-signing it by quorum
        self.check_mempool_result(tx=asset_unlock_tx_duplicate_index,
                result_expected={'allowed': False, 'reject-reason' : 'bad-assetunlock-not-verified'})

        self.log.info("Validating payload quorum selection")
        asset_unlock_tx_payload = CAssetUnlockTx()
        asset_unlock_tx_payload.deserialize(BytesIO(asset_unlock_tx.vExtraPayload))

        request_id = self.create_assetunlock_request_id(101)
        assert_equal(asset_unlock_tx_payload.quorumHash, int(self.mninfo[0].get_node(self).quorum("selectquorum", llmq_type_test, request_id)["quorumHash"], 16))

        txid = self.send_tx(asset_unlock_tx)

        self.log.info("Test RPC getassetunlockstatuses part I")
        tip = self.nodes[0].getblockcount()
        indexes_statuses_no_height = self.nodes[0].getassetunlockstatuses(["101", "102", "300"])
        assert_equal([{'index': 101, 'status': 'mempooled', 'instantlock': False}, {'index': 102, 'status': 'unknown'}, {'index': 300, 'status': 'unknown'}], indexes_statuses_no_height)
        indexes_statuses_height = self.nodes[0].getassetunlockstatuses(["101", "102", "300"], tip)
        assert_equal([{'index': 101, 'status': 'unknown'}, {'index': 102, 'status': 'unknown'}, {'index': 300, 'status': 'unknown'}], indexes_statuses_height)


        self.log.info("Test no IS for asset unlock...")
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 0)
        self.wait_for_sporks_same()

        assert_equal(node.getmempoolentry(txid)['fees']['base'], Decimal("0.0007"))
        is_id = node_wallet.sendtoaddress(node_wallet.getnewaddress(), 1)
        self.wait_for_instantlock(is_id)

        rawtx = node.getrawtransaction(txid, 1)
        rawtx_is = node.getrawtransaction(is_id, 1)
        assert_equal(rawtx["instantlock"], False)
        assert_equal(rawtx_is["instantlock"], True)
        assert_equal(rawtx["chainlock"], False)
        assert_equal(rawtx_is["chainlock"], False)
        assert not "confirmations" in rawtx
        assert not "confirmations" in rawtx_is
        self.log.info("Reset IS spork")
        self.set_sporks()

        assert "assetUnlockTx" in node.getrawtransaction(txid, 1)

        self.mempool_size += 2
        self.check_mempool_size()
        self.validate_credit_pool_balance(locked)
        self.generate(node, 1)
        rawtx = node.getrawtransaction(txid, 1)
        assert_equal(rawtx["confirmations"], 1)
        self.validate_credit_pool_balance(locked - COIN)
        self.mempool_size -= 2
        self.check_mempool_size()
        block_asset_unlock = node.getrawtransaction(asset_unlock_tx.rehash(), 1)['blockhash']
        self.log.info("Checking rpc `getblock` and `getblockstats` succeeds as they use own fee calculation mechanism")
        assert_equal(node.getblockstats(node.getblockcount())['maxfee'], tiny_amount)
        node.getblock(block_asset_unlock, 2)

        self.send_tx(asset_unlock_tx,
            expected_error = "Transaction already in block chain",
            reason = "double copy")

        self.log.info("Mining next quorum to check tx 'asset_unlock_tx_late' is still valid...")
        self.mine_quorum_2_nodes()
        self.log.info("Checking credit pool amount is same...")
        self.validate_credit_pool_balance(locked - 1 * COIN)
        self.check_mempool_result(tx=asset_unlock_tx_late, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})
        self.log.info("Checking credit pool amount still is same...")
        self.validate_credit_pool_balance(locked - 1 * COIN)
        self.send_tx(asset_unlock_tx_late)
        self.generate(node, 1)
        self.validate_credit_pool_balance(locked - 2 * COIN)

        self.log.info("Generating many blocks to make quorum far behind (even still active)...")
        self.generate_batch(too_late_height - node.getblockcount() - 1)
        self.check_mempool_result(tx=asset_unlock_tx_too_late, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})
        self.generate(node, 1)
        self.check_mempool_result(tx=asset_unlock_tx_too_late,
                result_expected={'allowed': False, 'reject-reason' : 'bad-assetunlock-too-late'})
        self.log.info("A peer relaying an unlock outside the tip's height window is not punished")
        # Validity depends on the tip, so an honest peer a block behind relays it. The
        # Misbehaving line is logged even for this whitelisted (noban) peer.
        late_peer = node.add_p2p_connection(P2PInterface())
        with node.assert_debug_log(expected_msgs=["bad-assetunlock-too-late"], unexpected_msgs=["Misbehaving"]):
            late_peer.send_and_ping(msg_tx(asset_unlock_tx_too_late))
        node.disconnect_p2ps()

        block_to_reconsider = node.getbestblockhash()
        self.log.info("Test block invalidation with asset unlock tx...")
        for inode in self.nodes:
            inode.invalidateblock(block_asset_unlock)
        self.validate_credit_pool_balance(locked)
        self.generate_batch(25, sync_fun=lambda: self.sync_blocks())
        self.validate_credit_pool_balance(locked)
        for inode in self.nodes:
            inode.reconsiderblock(block_to_reconsider)
        self.validate_credit_pool_balance(locked - 2 * COIN)

        self.generate(node, 1)

        self.validate_credit_pool_balance(locked - 2 * COIN)
        self.validate_credit_pool_balance(block_hash=self.block_hash_1, expected=locked)

        self.log.info("Forcibly mine asset_unlock_tx_duplicate_index and ensure block is invalid")
        self.create_and_check_block([asset_unlock_tx_duplicate_index], expected_error = "bad-assetunlock-duplicated-index")

    def test_withdrawal_limits(self, node_wallet, node, pubkey):
        self.log.info("Testing withdrawal limits before v22 'withdrawal fork'...")
        assert not softfork_active(node_wallet, 'withdrawals')

        self.log.info("Too big withdrawal is expected to not be mined")
        asset_unlock_tx_full = self.create_assetunlock(201, 1 + self.get_credit_pool_balance(), pubkey)

        self.log.info("Checking that transaction with exceeding amount accepted by mempool...")
        # Mempool doesn't know about the size of the credit pool
        self.check_mempool_result(tx=asset_unlock_tx_full, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})

        txid_in_block = self.send_tx(asset_unlock_tx_full)
        self.generate(node, 1)

        self.ensure_tx_is_not_mined(txid_in_block)

        self.log.info("Forcibly mine asset_unlock_tx_full and ensure block is invalid...")
        self.create_and_check_block([asset_unlock_tx_full], expected_error = "failed-creditpool-unlock-too-much")

        self.mempool_size += 1
        asset_unlock_tx_full = self.create_assetunlock(301, self.get_credit_pool_balance(), pubkey)
        self.check_mempool_result(tx=asset_unlock_tx_full, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})

        txid_in_block = self.send_tx(asset_unlock_tx_full)
        expected_balance = (Decimal(self.get_credit_pool_balance()) - Decimal(tiny_amount))
        self.generate(node, 1)
        self.log.info("Check txid_in_block was mined")
        block = node.getblock(node.getbestblockhash())
        assert txid_in_block in block['tx']
        self.validate_credit_pool_balance(0)

        self.log.info(f"Check status of withdrawal and try to spend it")
        withdrawal_status = node_wallet.gettransaction(txid_in_block)
        assert_equal(withdrawal_status['amount'] * COIN, expected_balance)
        assert_equal(withdrawal_status['details'][0]['category'], 'platform-transfer')

        spend_withdrawal_hex = node_wallet.createrawtransaction([{'txid': txid_in_block, 'vout' : 0}], { node_wallet.getnewaddress() : (expected_balance - Decimal(tiny_amount)) / COIN})
        spend_withdrawal_hex = node_wallet.signrawtransactionwithwallet(spend_withdrawal_hex)['hex']
        spend_withdrawal = tx_from_hex(spend_withdrawal_hex)
        self.check_mempool_result(tx=spend_withdrawal, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})
        spend_txid_in_block = self.send_tx(spend_withdrawal)

        self.generate(node, 1, sync_fun=self.no_op)
        block = node.getblock(node.getbestblockhash())
        assert spend_txid_in_block in block['tx']

        self.log.info("Fast forward to the next day to reset all current unlock limits...")
        self.generate_batch(blocks_in_one_day)
        self.mine_quorum_2_nodes()

        total = self.get_credit_pool_balance()
        coins = node_wallet.listunspent()
        while total <= 10_901 * COIN:
            if len(coins) == 0:
                coins = node_wallet.listunspent(query_options={'minimumAmount': 1})
            coin = coins.pop()
            to_lock = int(coin['amount'] * COIN) - tiny_amount
            if to_lock > 99 * COIN and total > 10_000 * COIN:
                to_lock = 99 * COIN

            total += to_lock
            tx = self.create_assetlock(coin, to_lock, pubkey)
            self.send_tx_simple(tx)
            self.log.info(f"Collecting coins in pool... Collected {total}/{10_901 * COIN}")
        self.sync_mempools()
        self.generate(node, 1)
        credit_pool_balance_1 = self.get_credit_pool_balance()
        assert_greater_than(credit_pool_balance_1, 10_901 * COIN)
        limit_amount_1 = 1000 * COIN
        self.log.info("Create 5 transactions and make sure that only 4 of them can be mined")
        self.log.info("because their sum is bigger than the hard-limit (1000)")
        # take most of limit by one big tx for faster testing and
        # create several tiny withdrawal with exactly 1 *invalid* / causes spend above limit tx
        withdrawals = [600 * COIN, 100 * COIN, 100 * COIN, 100 * COIN - 10000, 100 * COIN + 10001]
        amount_to_withdraw_1 = sum(withdrawals)
        index = 400
        for next_amount in withdrawals:
            index += 1
            asset_unlock_tx = self.create_assetunlock(index, next_amount, pubkey)
            last_txid = self.send_tx_simple(asset_unlock_tx)
            # make sure larger amounts are mined first simply to make this test deterministic
            node.prioritisetransaction(last_txid, next_amount // 10000)

        self.sync_mempools()
        self.generate(node, 1)

        new_total = self.get_credit_pool_balance()
        amount_actually_withdrawn = total - new_total
        self.log.info("Testing that we tried to withdraw more than we could")
        assert_greater_than(amount_to_withdraw_1, amount_actually_withdrawn)
        self.log.info("Checking that we tried to withdraw more than the hard-limit (1000)")
        assert_greater_than(amount_to_withdraw_1, limit_amount_1)
        self.log.info("Checking we didn't actually withdraw more than allowed by the limit")
        assert_greater_than_or_equal(limit_amount_1, amount_actually_withdrawn)
        assert_equal(amount_actually_withdrawn, 900 * COIN + 10001)

        self.generate(node, 1)
        self.log.info("Checking that exactly 1 tx stayed in mempool...")
        self.mempool_size = 1
        self.check_mempool_size()
        assert_equal(new_total, self.get_credit_pool_balance())
        pending_txid = node.getrawmempool()[0]

        amount_to_withdraw_2 = limit_amount_1 - amount_actually_withdrawn
        self.log.info(f"We can still consume {Decimal(str(amount_to_withdraw_2 / COIN))} before we hit the hard-limit (1000)")
        index += 1
        asset_unlock_tx = self.create_assetunlock(index, amount_to_withdraw_2, pubkey)
        self.send_tx_simple(asset_unlock_tx)
        self.sync_mempools()
        self.generate(node, 1)
        new_total = self.get_credit_pool_balance()
        amount_actually_withdrawn = total - new_total
        assert_equal(limit_amount_1, amount_actually_withdrawn)

        self.log.info("Checking that exactly the same tx as before stayed in mempool and it's the only one...")
        self.mempool_size = 1
        self.check_mempool_size()
        assert_equal(new_total, self.get_credit_pool_balance())
        assert pending_txid in node.getrawmempool()

        self.log.info("Fast forward to next day again...")
        self.generate_batch(blocks_in_one_day - 1)
        self.log.info("Checking mempool is empty now...")
        self.mempool_size = 0
        self.check_mempool_size()

        self.log.info("Creating new asset-unlock tx. It should be mined exactly 1 block after")
        credit_pool_balance_2 = self.get_credit_pool_balance()
        limit_amount_2 = credit_pool_balance_2 // 10
        index += 1
        asset_unlock_tx = self.create_assetunlock(index, limit_amount_2, pubkey)
        self.send_tx(asset_unlock_tx)
        self.generate(node, 1)
        assert_equal(new_total, self.get_credit_pool_balance())
        self.generate(node, 1)
        new_total -= limit_amount_2
        assert_equal(new_total, self.get_credit_pool_balance())
        self.log.info("Trying to withdraw more... expecting to fail")
        index += 1
        asset_unlock_tx = self.create_assetunlock(index, COIN, pubkey)
        self.send_tx(asset_unlock_tx)
        self.generate(node, 1)

        tip = self.nodes[0].getblockcount()
        indexes_statuses_no_height = self.nodes[0].getassetunlockstatuses(["101", "102", "103"])
        assert_equal([{'index': 101, 'status': 'mined'}, {'index': 102, 'status': 'mined'}, {'index': 103, 'status': 'unknown'}], indexes_statuses_no_height)
        indexes_statuses_height = self.nodes[0].getassetunlockstatuses(["101", "102", "103"], tip)
        assert_equal([{'index': 101, 'status': 'chainlocked'}, {'index': 102, 'status': 'chainlocked'}, {'index': 103, 'status': 'unknown'}], indexes_statuses_height)


        self.log.info("generate many blocks to be sure that mempool is empty after expiring txes...")
        self.generate_batch(HEIGHT_DIFF_EXPIRING)
        self.log.info("Checking that credit pool is not changed...")
        assert_equal(new_total, self.get_credit_pool_balance())
        self.check_mempool_size()
        assert not softfork_active(node_wallet, 'withdrawals')

    def test_mn_rr(self, node_wallet, node, pubkey):
        self.log.info("Activate mn_rr...")
        locked = self.get_credit_pool_balance()
        self.activate_mn_rr()
        self.log.info(f'mn-rr height: {node.getblockcount()} credit: {self.get_credit_pool_balance()}')
        assert_equal(locked, self.get_credit_pool_balance())

        bt = node.getblocktemplate()
        platform_reward = bt['masternode'][0]['amount']
        assert_equal(bt['masternode'][0]['script'], '6a')  # empty OP_RETURN
        owner_reward = bt['masternode'][1]['amount']
        operator_reward = bt['masternode'][2]['amount'] if len(bt['masternode']) == 3 else 0
        all_mn_rewards = platform_reward + owner_reward + operator_reward
        assert_equal(all_mn_rewards, bt['coinbasevalue'] * 3 // 4)  # 75/25 mn/miner reward split
        assert_equal(platform_reward, all_mn_rewards * 375 // 1000)  # 0.375 platform share
        assert_equal(platform_reward, 112592247)
        assert_equal(locked, self.get_credit_pool_balance())
        self.generate(node, 1)
        locked += platform_reward
        assert_equal(locked, self.get_credit_pool_balance())

        coins = node_wallet.listunspent(query_options={'minimumAmount': 1})
        coin = coins.pop()
        self.send_tx(self.create_assetlock(coin, COIN, pubkey))
        locked += platform_reward + COIN
        self.generate(node, 1)
        assert_equal(locked, self.get_credit_pool_balance())

    def test_withdrawals_fork(self, node_wallet, node, pubkey):
        self.log.info("Testing asset unlock after 'withdrawals' activation...")
        self.activate_by_name('withdrawals', 600)
        assert softfork_active(node_wallet, 'withdrawals')
        self.log.info(f'post-withdrawals height: {node.getblockcount()} credit: {self.get_credit_pool_balance()}')

        asset_unlock_tx, asset_unlock_tx_payload, quorumHash_str = self.create_assetunlock_for_oldest_quorum(501, COIN, pubkey)
        self.log.info("Check that Asset Unlock tx is valid for current quorum")
        self.check_mempool_result(tx=asset_unlock_tx, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})

        assert quorumHash_str in node_wallet.quorum('list')['llmq_test_platform']
        self.log.info("Generate one more quorum to make signing quorum inactive but still valid")
        self.mine_quorum_2_nodes()
        assert quorumHash_str not in node_wallet.quorum('list')['llmq_test_platform']

        assert asset_unlock_tx_payload.requestedHeight + HEIGHT_DIFF_EXPIRING > node_wallet.getblockcount()
        self.check_mempool_result(tx=asset_unlock_tx, result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})

        self.log.info("Generate one more quorum after which signing quorum becomes too old")
        self.mine_quorum_2_nodes()
        self.check_mempool_result(tx=asset_unlock_tx, result_expected={'allowed': False, 'reject-reason': 'bad-assetunlock-too-old-quorum'})

        asset_unlock_tx = self.create_assetunlock(520, 2000 * COIN + 1, pubkey)
        txid_in_block = self.send_tx(asset_unlock_tx)
        self.generate(node, 1)
        self.ensure_tx_is_not_mined(txid_in_block)

        asset_unlock_tx = self.create_assetunlock(521, 2000 * COIN, pubkey)
        txid_in_block = self.send_tx(asset_unlock_tx)
        self.generate(node, 1)
        block = node.getblock(node.getbestblockhash())
        assert txid_in_block in block['tx']

        asset_unlock_tx = self.create_assetunlock(522, COIN, pubkey)
        txid_in_block = self.send_tx(asset_unlock_tx)
        self.generate(node, 1)
        self.ensure_tx_is_not_mined(txid_in_block)

    def test_v24_fork(self, node_wallet, node, pubkey):
        self.log.info("Testing asset unlock after 'v24' activation...")
        self.activate_by_name('v24', 750)
        self.mempool_size = node_wallet.getmempoolinfo()['size']
        self.log.info(f'post-v24 height: {node.getblockcount()} credit: {self.get_credit_pool_balance()}')

        self.test_asset_locks_v2(node_wallet, node, pubkey)

        self.test_admissible_asset_unlock_ancestor_package(node_wallet, pubkey)

        self.log.info("The v24 limit is 20% of the balance one window ago, at least 2000, net of the window's drop")
        limit = self.check_v24_unlock_limit(node)
        assert_greater_than(limit, 2000 * COIN)

        # Far over the limit, so the rewards of the next blocks cannot make it minable while it
        # waits in the mempool next to the unlocks below
        asset_unlock_tx = self.create_assetunlock(620, limit + 100 * COIN, pubkey)
        txid_in_block = self.send_tx(asset_unlock_tx)
        self.log.info(f"{txid_in_block} should not be mined")
        tip_hash = self.generate(node, 1)[0]
        assert txid_in_block not in node.getblock(tip_hash)['tx']

        # The block grew the pool by the Platform reward, which is withdrawable on top
        limit = self.check_v24_unlock_limit(node)
        asset_unlock_tx = self.create_assetunlock(621, limit, pubkey)
        txid_in_block = self.send_tx(asset_unlock_tx)
        self.log.info(f"{txid_in_block} should be mined")
        tip_hash = self.generate(node, 1)[0]
        assert txid_in_block in node.getblock(tip_hash)['tx']

        self.log.info("Deposits inside the window refill the limit: a lock is withdrawable again at once")
        limit = self.check_v24_unlock_limit(node)
        # Only the flow of one block is left: the Platform reward the pool gained, less what the
        # window start moved
        lock_amount = 10 * COIN
        assert_greater_than(lock_amount // 2, limit)
        coin = node_wallet.listunspent(query_options={'minimumAmount': 11}).pop()
        lock_tx = self.create_assetlock(coin, lock_amount, pubkey, version=2)
        self.send_tx(lock_tx)
        self.generate(node, 1)
        refilled_limit = self.check_v24_unlock_limit(node)
        # The window start moved one block too, so the balance it compares against shifted by
        # that block's own flow; the lock itself is fully withdrawable again
        assert_greater_than_or_equal(refilled_limit, lock_amount + limit - 2 * COIN)
        assert_greater_than(refilled_limit, limit)
        asset_unlock_tx = self.create_assetunlock(623, refilled_limit, pubkey)
        txid_in_block = self.send_tx(asset_unlock_tx)
        self.log.info(f"{txid_in_block} should be mined")
        tip_hash = self.generate(node, 1)[0]
        assert txid_in_block in node.getblock(tip_hash)['tx']

        limit = self.check_v24_unlock_limit(node)
        # index 622 was skipped above so that this unlock, which is never mined, is the one the
        # ancestor package test below holds on to; far over the limit for the same reason as 620
        asset_unlock_tx = self.create_assetunlock(622, limit + 100 * COIN, pubkey)
        txid_in_block = self.send_tx(asset_unlock_tx)
        self.log.info(f"{txid_in_block} should not be mined")
        tip_hash = self.generate(node, 1)[0]
        assert txid_in_block not in node.getblock(tip_hash)['tx']

        self.test_asset_unlock_ancestor_package(node_wallet, asset_unlock_tx, txid_in_block)

    def check_v24_unlock_limit(self, node):
        tip = node.getblockcount()
        window_start = tip - blocks_in_one_day
        balance = self.get_credit_pool_balance()
        window_start_balance = self.get_credit_pool_balance(block_hash=node.getblockhash(window_start))
        allowed_drop = max(window_start_balance * 20 // 100, 2000 * COIN)
        expected_limit = min(max(allowed_drop - (window_start_balance - balance), 0), balance)
        expected = {
            'height': tip,
            'blockhash': node.getbestblockhash(),
            'balance': Decimal(balance) / COIN,
            'currentlimit': Decimal(expected_limit) / COIN,
            'window': {
                'blocks': blocks_in_one_day,
                'height': window_start,
                'balance': Decimal(window_start_balance) / COIN,
                'unlocked': node.getcreditpoolinfo()['window']['unlocked'],
            },
        }
        assert_equal(node.getcreditpoolinfo(), expected)
        assert_equal(node.getcreditpoolinfo(tip), expected)
        assert_raises_rpc_error(-8, "Block height out of range", node.getcreditpoolinfo, tip + 1)
        assert_raises_rpc_error(-8, "Block height out of range", node.getcreditpoolinfo, -1)
        return expected_limit

    def test_admissible_asset_unlock_ancestor_package(self, node_wallet, pubkey):
        self.log.info("Test an admissible Asset Unlock ancestor package")
        asset_unlock_tx = self.create_assetunlock(619, COIN, pubkey)
        asset_unlock_txid = self.send_tx(asset_unlock_tx)
        child_txid = self.create_asset_unlock_child(node_wallet, asset_unlock_tx, asset_unlock_txid)

        template_txids = {tx_from_hex(tx['data']).rehash() for tx in node_wallet.getblocktemplate()['transactions']}
        assert asset_unlock_txid in template_txids
        assert child_txid in template_txids

        tip_hash = self.generate(node_wallet, 1)[0]
        mined_txids = node_wallet.getblock(tip_hash)['tx']
        assert asset_unlock_txid in mined_txids
        assert child_txid in mined_txids

    def test_asset_unlock_ancestor_package(self, node_wallet, asset_unlock_tx, asset_unlock_txid):
        self.log.info("Test an Asset Unlock that exceeds the current limit as an ancestor package")
        child_txid = self.create_asset_unlock_child(node_wallet, asset_unlock_tx, asset_unlock_txid)

        template_txids = {tx_from_hex(tx['data']).rehash() for tx in node_wallet.getblocktemplate()['transactions']}
        assert asset_unlock_txid not in template_txids
        assert child_txid not in template_txids

    def test_asset_locks_v2_pre_v24(self, node_wallet, node, pubkey):
        self.log.info("Testing asset lock v2 rejection before v24 activation...")
        assert not softfork_active(node_wallet, 'v24')

        self.mempool_size = node_wallet.getmempoolinfo()['size']

        coins = node_wallet.listunspent(query_options={'minimumAmount': 1})
        coin = coins.pop()
        lock_tx = self.create_assetlock(coin, COIN, pubkey, version=2)
        self.check_mempool_result(tx=lock_tx,
            result_expected={'allowed': False, 'reject-reason': 'bad-assetlocktx-version-2'})
        self.log.info("v2 asset lock correctly rejected pre-v24")

        self.log.info("Testing asset unlock v2 rejection before v24 activation...")
        unlock_tx_v2 = self.create_assetunlock(590, COIN, pubkey, version=2)
        # The stable-txid hashing rule applies to any v2 payload, even pre-fork rejected ones
        self.check_mempool_result(tx=unlock_tx_v2, txid=self.get_v2_txid(unlock_tx_v2),
            result_expected={'allowed': False, 'reject-reason': 'bad-assetunlocktx-version-2'})
        self.log.info("v2 asset unlock correctly rejected pre-v24")

        self.log.info("Spending RPCs refuse to pay a Platform address before the fork")
        assert_raises_rpc_error(-8, "only valid after v24 activation", node_wallet.sendtoaddress,
                                encode_platform_p2pkh('tdash', hash160(pubkey)), 1)
        assert_raises_rpc_error(-8, "only valid after v24 activation", node_wallet.sendmany, "",
                                {encode_platform_p2pkh('tdash', hash160(pubkey)): 1})

    def test_asset_locks_v2(self, node_wallet, node, pubkey):
        self.log.info("Testing asset lock v2 after v24 activation...")
        assert softfork_active(node_wallet, 'v24')

        locked = self.get_credit_pool_balance()

        self.log.info("Test v2 mempool acceptance, sendtoaddress, v1 compat, validateaddress...")
        coins = node_wallet.listunspent(query_options={'minimumAmount': 1})

        # v2 via raw construction
        lock_tx_v2 = self.create_assetlock(coins.pop(), COIN, pubkey, version=2)
        self.check_mempool_result(tx=lock_tx_v2,
            result_expected={'allowed': True, 'fees': {'base': Decimal(str(tiny_amount / COIN))}})
        self.send_tx(lock_tx_v2)

        # v2 via sendtoaddress (two different platform addresses)
        platform_addr_from_key = encode_platform_p2pkh('tdash', hash160(pubkey))
        txid1 = node_wallet.sendtoaddress(platform_addr_from_key, 1.0)

        self.log.info("Test subtractfeefromamount for platform sendtoaddress")
        try:
            node_wallet.sendtoaddress(
                address=platform_addr_from_key,
                amount=1,
                subtractfeefromamount=True,
                fee_rate=1000,
            )
            raise AssertionError("subtracfeefromamount for platform address expected to generate error")
        except JSONRPCException as e:
            assert "subtractfeefromamount is not supported for Platform addresses" in e.error['message']

        self.log.info("Test a Platform recipient must get at least Platform's funding minimum")
        below_min_error = "Amount for a Platform address must be at least 0.00056"
        assert_raises_rpc_error(-6, below_min_error, node_wallet.sendtoaddress,
                                platform_addr_from_key, Decimal("0.00055999"))
        assert_raises_rpc_error(-6, below_min_error, node_wallet.sendmany, "",
                                {platform_addr_from_key: 0, node_wallet.getnewaddress(): 1})
        node_wallet.sendtoaddress(platform_addr_from_key, Decimal("0.00056"))

        val = node_wallet.validateaddress(platform_addr_from_key)
        assert_equal(val['isvalid'], True)
        assert_equal(val['isplatform'], True)

        # v1 still accepted post-v24
        lock_tx_v1 = self.create_assetlock(coins.pop(), COIN, pubkey, version=1)
        self.send_tx(lock_tx_v1)

        # mine all at once
        locked += node.getblocktemplate()['masternode'][0]['amount'] + 3 * COIN + 56000
        self.generate(node, 1)
        assert_equal(self.get_credit_pool_balance(), locked)

        # verify v2 raw tx JSON
        rpc_v2 = node.getrawtransaction(lock_tx_v2.rehash(), 1)
        assert_equal(rpc_v2['assetLockTx']['version'], 2)
        assert rpc_v2['assetLockTx']['creditOutputs'][0]['address'].startswith('tdash1')

        # verify v2 sendtoaddress tx JSON
        raw1 = node.getrawtransaction(txid1, 1)
        assert_equal(raw1['type'], 8)
        assert_equal(raw1['assetLockTx']['version'], 2)
        assert_equal(raw1['assetLockTx']['creditOutputs'][0]['valueSat'], COIN)
        assert_equal(raw1['assetLockTx']['creditOutputs'][0]['address'], platform_addr_from_key)
        assert any(vout['scriptPubKey']['type'] == 'nulldata' and vout['valueSat'] == COIN for vout in raw1['vout'])

        # verify v1 has no 'address' in JSON
        rpc_v1 = node.getrawtransaction(lock_tx_v1.rehash(), 1)
        assert_equal(rpc_v1['assetLockTx']['version'], 1)
        assert 'address' not in rpc_v1['assetLockTx']['creditOutputs'][0]


if __name__ == '__main__':
    AssetLocksTest().main()
