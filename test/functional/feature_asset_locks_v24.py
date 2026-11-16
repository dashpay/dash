#!/usr/bin/env python3

# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Test version 2 asset unlocks, non-standard asset locks and crash replay.

Covers version 2 asset unlocks with stable txids, the non-standardness of
version 2 and >100-input asset locks, and recovery from a crash mid-flush that
leaves an asset unlock to replay. Everything else about asset locks, through
the v24 fork, is in feature_asset_locks.py.
"""

import copy
import http.client
import subprocess
from decimal import Decimal
from io import BytesIO

from test_framework.asset_locks import (
    AssetLocksMixin,
    HEIGHT_DIFF_EXPIRING,
    blocks_in_one_day,
    llmq_type_test,
    tiny_amount,
)
from test_framework.messages import (
    CAssetUnlockTx,
    CCoinJoinBroadcastTx,
    COIN,
    MSG_ASSET_UNLOCK,
    msg_dstx,
    msg_tx,
    tx_from_hex,
)
from test_framework.p2p import P2PInterface
from test_framework.script import hash160
from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises,
    assert_raises_rpc_error,
    softfork_active,
)
from test_framework.segwit_addr import encode_platform_p2pkh
from test_framework.wallet_util import generate_keypair

ASSET_UNLOCK_TX_TYPE = 9 # TRANSACTION_ASSET_UNLOCK
V24_ACTIVATION_HEIGHT = 250
CREDIT_POOL_FUNDING = 5000 * COIN


class InvListener(P2PInterface):
    def __init__(self):
        super().__init__()
        self.asset_unlock_invs = []

    def on_inv(self, message):
        for i in message.inv:
            if i.type == MSG_ASSET_UNLOCK:
                self.asset_unlock_invs.append(i.hash)


class AssetLocksV24Test(AssetLocksMixin, DashTestFramework):
    def set_test_params(self):
        # v24 is miner-activated here: the 250-block EHF window of the shipped deployment would
        # cost minutes of mining, and the scenarios do not depend on how the fork was signalled
        self.set_asset_locks_test_params([
            f"-vbparams=v24:0:999999999999:{V24_ACTIVATION_HEIGHT}:10:8:6:5:0",
        ])

    def run_test(self):
        node_wallet = self.nodes[0]
        node = self.nodes[1]

        self.set_sporks()

        assert_equal(self.nodes[0].getdeploymentinfo()['deployments']['v20']['active'], True)

        for _ in range(2):
            self.dynamically_add_masternode(evo=True)

        privkey, pubkey = generate_keypair(wif=True)
        node_wallet.importprivkey(privkey)

        self.fund_credit_pool(node_wallet, node, pubkey)

        self.log.info("Mine IS and platform quorums")
        self.mine_quorum(llmq_type_name='llmq_test_instantsend', expected_members=2, expected_connections=1, expected_contributions=2, expected_commitments=2, llmq_type=104)
        self.mine_quorum_2_nodes()

        self.activate_by_name('v24')
        self.mine_quorum_2_nodes()
        self.exhaust_unlock_window(node_wallet, node, pubkey)

        self.test_asset_unlock_v2(node_wallet, node, pubkey)
        self.test_non_standard(node_wallet, node, pubkey)
        self.test_replay_after_crash(node_wallet, node, pubkey)

    def fund_credit_pool(self, node_wallet, node, pubkey):
        self.log.info("Fund the credit pool with a single asset lock, a whole withdrawal window before v24 is active")
        coins = []
        total = 0
        for coin in node_wallet.listunspent():
            coins.append(coin)
            total += int(coin['amount'] * COIN)
            if total > CREDIT_POOL_FUNDING + COIN:
                break
        assert_greater_than(total, CREDIT_POOL_FUNDING + COIN)
        self.send_tx(self.create_assetlock(coins, CREDIT_POOL_FUNDING, pubkey))
        self.generate(node, 1)
        self.funding_height = node.getblockcount()
        assert_greater_than_or_equal(int(node.getcreditpoolinfo()['balance'] * COIN), CREDIT_POOL_FUNDING)

    def current_limit(self, node):
        return int(node.getcreditpoolinfo()['currentlimit'] * COIN)

    def exhaust_unlock_window(self, node_wallet, node, pubkey):
        self.log.info("Exhaust the withdrawal limit and leave an over-limit unlock with a child in the mempool")
        # A deposit inside the window would be withdrawable in full instead of the window's allowed drop
        assert_greater_than(node.getblockcount() - blocks_in_one_day, self.funding_height)
        # Far over the limit, so the rewards of the next blocks cannot make it minable
        over_limit_tx = self.create_assetunlock(620, self.current_limit(node) + 100 * COIN, pubkey)
        txid = self.send_tx(over_limit_tx)
        tip_hash = self.generate(node, 1)[0]
        assert txid not in node.getblock(tip_hash)['tx']

        txid = self.send_tx(self.create_assetunlock(621, self.current_limit(node), pubkey))
        tip_hash = self.generate(node, 1)[0]
        assert txid in node.getblock(tip_hash)['tx']
        assert_equal(self.current_limit(node), 0)

        over_limit_tx = self.create_assetunlock(622, 100 * COIN, pubkey)
        txid = self.send_tx(over_limit_tx)
        tip_hash = self.generate(node, 1)[0]
        assert txid not in node.getblock(tip_hash)['tx']
        self.create_asset_unlock_child(node_wallet, over_limit_tx, txid)

    def test_asset_unlock_v2(self, node_wallet, node, pubkey):
        self.log.info("Testing v2 asset unlocks with stable txids...")
        assert softfork_active(node_wallet, 'v24')
        self.log.info("Enable InstantSend: version 2 unlocks are locked once they are minable within the limit")
        node_wallet.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 0)
        self.wait_for_sporks_same()

        # The withdrawal window is exhausted by exhaust_unlock_window, so v2 instances created here stay
        # unminable in the mempool until the window clears
        index = 800
        listener = node_wallet.add_p2p_connection(InvListener())
        # A peer at the current protocol version supplies the first instance; refreshes of the
        # withdrawal must still be announced to it later
        sender = node_wallet.add_p2p_connection(InvListener())
        unlock_a = self.create_assetunlock(index, COIN, pubkey, version=2)
        stable_txid = self.get_v2_txid(unlock_a)
        instance_a = unlock_a.rehash()
        assert stable_txid != instance_a

        sender.send_and_ping(msg_tx(unlock_a))
        self.wait_until(lambda: stable_txid in node_wallet.getrawmempool())
        self.sync_mempools()
        rpc_tx = node_wallet.getrawtransaction(stable_txid, 1)
        assert_equal(rpc_tx['txid'], stable_txid)
        assert_equal(rpc_tx['instanceHash'], instance_a)

        self.log.info("The v2 unlock is announced by instance hash")
        self.wait_until(lambda: int(instance_a, 16) in listener.asset_unlock_invs)

        self.log.info("Pending withdrawals exceed the limit, so the unlock is not locked while an ordinary tx is")
        pending = node_wallet.getmempoolinfo()['pendingassetunlocks']
        # the over-limit unlocks left by exhaust_unlock_window exceed a whole window's allowed drop
        info = node_wallet.getcreditpoolinfo()
        allowed_drop = max(info['window']['balance'] * 20 / 100, 2000)
        assert_greater_than(pending, allowed_drop)
        assert_greater_than(pending, info['currentlimit'])
        is_txid = node_wallet.sendtoaddress(node_wallet.getnewaddress(), 1)
        self.wait_for_instantlock(is_txid)
        assert_equal(node_wallet.getrawtransaction(stable_txid, 1)['instantlock'], False)
        assert_equal(node_wallet.getassetunlockstatuses([str(index)])[0], {'index': index, 'status': 'mempooled', 'instantlock': False})

        self.log.info("A second withdrawal refused on the limit is retried automatically once the window clears")
        retry_index = 801
        unlock_retry = self.create_assetunlock(retry_index, COIN, pubkey, version=2)
        retry_txid = self.get_v2_txid(unlock_retry)
        assert_equal(self.send_tx_simple(unlock_retry), retry_txid)
        self.sync_mempools()
        assert_equal(node_wallet.getrawtransaction(retry_txid, 1)['instantlock'], False)

        self.log.info("Spend the unmined v2 unlock by its stable txid")
        child_value = Decimal(unlock_a.vout[0].nValue - tiny_amount) / COIN
        child_hex = node_wallet.createrawtransaction(
            [{'txid': stable_txid, 'vout': 0}],
            {node_wallet.getnewaddress(): child_value})
        signed_child = node_wallet.signrawtransactionwithwallet(child_hex)
        assert signed_child['complete']
        child_txid = node_wallet.sendrawtransaction(signed_child['hex'])
        self.sync_mempools()
        assert child_txid in node_wallet.getrawmempool()

        def child_output():
            return next(u for u in node_wallet.listunspent(0) if u['txid'] == child_txid)
        self.log.info("Without a lock on the withdrawal the wallet does not trust the child's output")
        assert_equal(child_output()['safe'], False)

        self.log.info("A fresher re-signed instance refreshes the entry in place; same txid, child untouched")
        self.generate(node, 1)
        unlock_a2 = self.create_assetunlock(index, COIN, pubkey, version=2)
        assert_equal(self.get_v2_txid(unlock_a2), stable_txid)
        instance_a2 = unlock_a2.rehash()
        assert instance_a2 != instance_a
        assert_equal(self.send_tx_simple(unlock_a2), stable_txid)
        self.sync_unlock_instance(stable_txid, instance_a2)
        mempool = node_wallet.getrawmempool()
        assert stable_txid in mempool
        assert child_txid in mempool
        self.log.info("The refresh is announced under its own instance hash, including to the peer that sent the first instance")
        self.wait_until(lambda: int(instance_a2, 16) in listener.asset_unlock_invs)
        self.wait_until(lambda: int(instance_a2, 16) in sender.asset_unlock_invs)
        assert int(instance_a, 16) not in sender.asset_unlock_invs

        self.log.info("A stale instance does not refresh a fresher one")
        assert_raises_rpc_error(-26, 'assetunlock-stale-instance', self.send_tx_simple, unlock_a)

        self.log.info("A stale instance in a package is rejected by the refresh path; the fresher held instance stays")
        stale_package_child_hex = node_wallet.createrawtransaction(
            [{'txid': stable_txid, 'vout': 0}],
            {node_wallet.getnewaddress(): child_value})
        signed_stale_package_child = node_wallet.signrawtransactionwithwallet(stale_package_child_hex)
        assert signed_stale_package_child['complete']
        assert_raises_rpc_error(-26, f'{stable_txid} failed: assetunlock-stale-instance', node_wallet.submitpackage,
                                [unlock_a.serialize().hex(), signed_stale_package_child['hex']])
        assert_equal(node_wallet.getrawtransaction(stable_txid, 1)['instanceHash'], instance_a2)
        assert child_txid in node_wallet.getrawmempool()

        self.log.info("Package test acceptance treats the held withdrawal as already in the mempool")
        # A multi-transaction testmempoolaccept must not route the mempooled unlock through the
        # in-place refresh path: that used to leave the package feerate accounting unfilled and
        # abort the node. Refreshes are only admitted through single-transaction submission.
        funded = node_wallet.fundrawtransaction(node_wallet.createrawtransaction([], {node_wallet.getnewaddress(): 1}))
        signed_sibling = node_wallet.signrawtransactionwithwallet(funded['hex'])
        assert signed_sibling['complete']
        package_result = node_wallet.testmempoolaccept([unlock_a2.serialize().hex(), signed_sibling['hex']])
        assert_equal(package_result[0]['txid'], stable_txid)
        assert_equal(package_result[0]['allowed'], False)
        assert_equal(package_result[0]['reject-reason'], 'txn-already-in-mempool')

        self.log.info("Expire the v2 instance; it and its child must survive in the mempool")
        self.generate_batch(HEIGHT_DIFF_EXPIRING + 1)
        mempool = node_wallet.getrawmempool()
        assert stable_txid in mempool
        assert child_txid in mempool
        assert retry_txid in mempool

        self.log.info("A node without the expired instances admits them and the child from a peer")
        expired_hexes = [node_wallet.getrawtransaction(txid) for txid in (stable_txid, retry_txid, child_txid)]
        # Stopping dumps node's mempool, which the -persistmempool=0 run neither loads nor overwrites
        self.restart_node(1, self.extra_args[1] + ["-persistmempool=0"])
        assert stable_txid not in node.getrawmempool()
        expired_peer = node.add_p2p_connection(P2PInterface())
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["Misbehaving"]):
            for tx_hex in expired_hexes:
                expired_peer.send_and_ping(msg_tx(tx_from_hex(tx_hex)))
        mempool = node.getrawmempool()
        assert stable_txid in mempool
        assert retry_txid in mempool
        assert child_txid in mempool

        self.log.info("An expired instance must still carry a signature valid at its requestedHeight")
        forged = tx_from_hex(expired_hexes[0])
        forged_payload = CAssetUnlockTx()
        forged_payload.deserialize(BytesIO(forged.vExtraPayload))
        forged_payload.index = 810
        forged.vExtraPayload = forged_payload.serialize()
        forged_peer = node.add_p2p_connection(P2PInterface())
        with node.assert_debug_log(expected_msgs=["bad-assetunlock-not-verified", "Misbehaving: peer=", "(0 -> 100)"]):
            forged_peer.send_and_ping(msg_tx(forged))
        node.disconnect_p2ps()

        self.log.info("A requestedHeight overflowing the height window is rejected as too late")
        overflow_unlock = self.create_assetunlock(812, COIN, pubkey, version=2, requested_height=2**32 - HEIGHT_DIFF_EXPIRING)
        assert_equal(node.testmempoolaccept([overflow_unlock.serialize().hex()])[0]['reject-reason'], 'bad-assetunlock-too-late')

        self.log.info("An expired instance signed by a quorum mined after its requestedHeight is admitted")
        late_index = 811
        late_quorum = self.mninfo[0].get_node(self).quorum("selectquorum", llmq_type_test, self.create_assetunlock_request_id(late_index))["quorumHash"]
        late_quorum_mined_height = node.getblock(node.quorum("info", llmq_type_test, late_quorum)["minedBlock"])["height"]
        late_quorum_unlock = self.create_assetunlock(late_index, COIN, pubkey, version=2, requested_height=late_quorum_mined_height - 1)
        blocks_to_expiry = late_quorum_mined_height - 1 + HEIGHT_DIFF_EXPIRING - node.getblockcount()
        if blocks_to_expiry > 0:
            # node is still disconnected from node_wallet after its restart above
            self.generate(node, blocks_to_expiry, sync_fun=self.no_op)
        assert_equal(node.testmempoolaccept([late_quorum_unlock.serialize().hex()])[0]['allowed'], True)

        self.log.info("The expired instances and the child survive a restart")
        self.restart_node(1, self.extra_args[1])
        mempool = node.getrawmempool()
        assert stable_txid in mempool
        assert retry_txid in mempool
        assert child_txid in mempool
        self.connect_nodes(1, 0)
        self.sync_all()

        self.log.info("Flush the leftover withdrawals and clear the window; the expired instance keeps waiting for a re-sign")
        # The pending unlocks of exhaust_unlock_window would otherwise consume the cleared window and
        # crowd this test's withdrawal out of the block. Mine until each of them is either mined
        # or expired and evicted; the expired instance under test cannot be mined and stays.

        def other_unlocks_pending():
            self.sync_mempools()
            return any(txid not in (stable_txid, retry_txid) and node_wallet.getrawtransaction(txid, 1)['type'] == ASSET_UNLOCK_TX_TYPE
                       for txid in node_wallet.getrawmempool())
        flushed = 0
        while other_unlocks_pending():
            self.generate(node, 1)
            flushed += 1
            assert flushed <= 2 * blocks_in_one_day, "leftover withdrawals never left the mempool"
        self.generate_batch(101)
        mempool = node_wallet.getrawmempool()
        assert stable_txid in mempool
        assert child_txid in mempool
        self.log.info("A whole window later the allowance has regenerated to at least the 2000 DASH floor")
        assert_greater_than_or_equal(node_wallet.getcreditpoolinfo()['currentlimit'], 2000)

        self.log.info("Both expired instances fit the cleared limit but are not minable, so neither is locked")
        assert_equal(node_wallet.getmempoolinfo()['pendingassetunlocks'], 2 * Decimal(unlock_a.vout[0].nValue + tiny_amount) / COIN)
        assert_equal(node_wallet.getrawtransaction(stable_txid, 1)['instantlock'], False)
        assert_equal(node_wallet.getrawtransaction(retry_txid, 1)['instantlock'], False)

        self.log.info("Refreshing the second withdrawal gets it locked: the per-block retry re-evaluates it against the cleared limit")
        unlock_retry_b = self.create_assetunlock(retry_index, COIN, pubkey, version=2)
        assert_equal(self.send_tx_simple(unlock_retry_b), retry_txid)
        self.sync_unlock_instance(retry_txid, unlock_retry_b.rehash())
        self.wait_for_instantlock(retry_txid)
        self.generate(node, 1)
        assert retry_txid not in node_wallet.getrawmempool()

        self.log.info("Refresh the expired instance with a fresh re-sign: minable and within the limit, it gets locked")
        unlock_b = self.create_assetunlock(index, COIN, pubkey, version=2)
        assert_equal(self.get_v2_txid(unlock_b), stable_txid)
        assert_equal(self.send_tx_simple(unlock_b), stable_txid)
        self.sync_unlock_instance(stable_txid, unlock_b.rehash())
        self.wait_for_instantlock(stable_txid)
        assert_equal(node_wallet.getassetunlockstatuses([str(index)])[0], {'index': index, 'status': 'mempooled', 'instantlock': True})
        self.log.info("The lock pins the withdrawal index as the unlock's single input")
        islock = node_wallet.getislocks([stable_txid])[0]
        assert_equal(islock['inputs'], [{'txid': self.create_assetunlock_request_id(index), 'vout': 0}])
        self.log.info("The child is an ordinary spend of a locked parent: it gets locked and the wallet trusts it")
        self.wait_for_instantlock(child_txid)
        assert_equal(child_output()['safe'], True)

        self.log.info("A peer relaying an older instance of the locked withdrawal does not strip the locks")
        # The rejected instance shares the txid of the locked one still held in the mempool
        stale_peer = node_wallet.add_p2p_connection(P2PInterface())
        with node_wallet.assert_debug_log([f"{stable_txid} from peer=", "was not accepted"]):
            stale_peer.send_and_ping(msg_tx(unlock_a2))
        assert_equal(node_wallet.getrawtransaction(stable_txid, 1)['instanceHash'], unlock_b.rehash())
        assert_equal(node_wallet.getrawtransaction(stable_txid, 1)['instantlock'], True)
        assert_equal(node_wallet.getrawtransaction(child_txid, 1)['instantlock'], True)

        self.log.info("Mine the withdrawal with its child")
        tip_hash = self.generate(node, 1)[0]
        block = node_wallet.getblock(tip_hash, 2)
        mined_txids = [t['txid'] for t in block['tx']]
        assert stable_txid in mined_txids
        assert child_txid in mined_txids
        # The coinbase commits to the mined instance's hash; a single leaf is its own merkle root
        assert_equal(block['cbTx']['merkleRootAssetUnlocks'], unlock_b.rehash())
        assert_equal(node_wallet.getassetunlockstatuses([str(index)])[0], {'index': index, 'status': 'mined'})
        child_rpc = node_wallet.getrawtransaction(child_txid, 1)
        assert_equal(child_rpc['vin'][0]['txid'], stable_txid)
        assert_equal(node_wallet.getrawtransaction(stable_txid, 1)['instantlock'], True)

        self.log.info("At most one instance of a withdrawal index is held: a version 1 instance signed one block earlier is the claimant")
        cross_index = 803
        tip_height = node_wallet.getblockcount()
        unlock_v1 = self.create_assetunlock(cross_index, COIN, pubkey, version=1, requested_height=tip_height - 1)
        v1_txid = self.send_tx_simple(unlock_v1)
        self.sync_mempools()

        self.log.info("A version 2 instance signed at the same height is stale: rejected before its signature is verified")
        unlock_v2_stale = self.create_assetunlock(cross_index, COIN, pubkey, version=2, requested_height=tip_height - 1)
        stale_result = node_wallet.testmempoolaccept([unlock_v2_stale.serialize().hex()])[0]
        assert_equal(stale_result['txid'], self.get_v2_txid(unlock_v2_stale))
        assert_equal(stale_result['allowed'], False)
        assert_equal(stale_result['reject-reason'], 'assetunlock-stale-instance')
        assert v1_txid in node_wallet.getrawmempool()

        unlock_v2_fresh = self.create_assetunlock(cross_index, COIN, pubkey, version=2, requested_height=tip_height)
        cross_txid = self.get_v2_txid(unlock_v2_fresh)
        self.log.info("A package cannot replace an unlock while spending the evicted claimant")
        package_child_hex = node_wallet.createrawtransaction(
            [{'txid': v1_txid, 'vout': 0}, {'txid': cross_txid, 'vout': 0}],
            {node_wallet.getnewaddress(): Decimal(unlock_v1.vout[0].nValue + unlock_v2_fresh.vout[0].nValue - tiny_amount) / COIN})
        fresh_prevout = {'txid': cross_txid, 'vout': 0, 'scriptPubKey': unlock_v2_fresh.vout[0].scriptPubKey.hex(),
                         'amount': Decimal(unlock_v2_fresh.vout[0].nValue) / COIN}
        signed_package_child = node_wallet.signrawtransactionwithwallet(package_child_hex, [fresh_prevout])
        assert signed_package_child['complete']
        assert_raises_rpc_error(-25, 'assetunlock-conflicting-package', node_wallet.submitpackage,
                                [unlock_v1.serialize().hex(), unlock_v2_fresh.serialize().hex(), signed_package_child['hex']])
        assert v1_txid in node_wallet.getrawmempool()
        assert cross_txid not in node_wallet.getrawmempool()

        self.log.info("Package test acceptance predicts that rejection rather than validating the child against the evicted claimant")
        evicted_child_hex = node_wallet.createrawtransaction(
            [{'txid': v1_txid, 'vout': 0}],
            {node_wallet.getnewaddress(): Decimal(unlock_v1.vout[0].nValue - tiny_amount) / COIN})
        signed_evicted_child = node_wallet.signrawtransactionwithwallet(evicted_child_hex)
        assert signed_evicted_child['complete']
        test_result = node_wallet.testmempoolaccept([unlock_v2_fresh.serialize().hex(), signed_evicted_child['hex']])
        assert_equal([r['txid'] for r in test_result], [cross_txid, node_wallet.decoderawtransaction(signed_evicted_child['hex'])['txid']])
        for r in test_result:
            assert_equal(r['package-error'], 'assetunlock-conflicting-package')
            assert 'allowed' not in r
        assert v1_txid in node_wallet.getrawmempool()
        assert cross_txid not in node_wallet.getrawmempool()

        self.log.info("A version 2 unlock wrapped in a dstx message goes through DSTX validation and is dropped")
        dstx_peer = node_wallet.add_p2p_connection(P2PInterface())
        wrapped = CCoinJoinBroadcastTx(tx=unlock_v2_fresh, m_protxHash=1, vchSig=b"\x01" * 96, sigTime=self.mocktime)
        with node_wallet.assert_debug_log(["Invalid DSTX structure", "invalid dstx"]):
            dstx_peer.send_and_ping(msg_dstx(wrapped))
        assert cross_txid not in node_wallet.getrawmempool()
        assert v1_txid in node_wallet.getrawmempool()

        self.log.info("Signed one block later, the version 2 instance is fresher: it replaces the version 1 claimant and gets locked")
        assert_equal(self.send_tx_simple(unlock_v2_fresh), cross_txid)
        self.sync_mempools()
        mempool = node_wallet.getrawmempool()
        assert cross_txid in mempool
        assert v1_txid not in mempool
        assert_equal(node_wallet.getmempoolinfo()['pendingassetunlocks'], Decimal(unlock_v2_fresh.vout[0].nValue + tiny_amount) / COIN)
        self.wait_for_instantlock(cross_txid)
        assert_equal(node_wallet.getassetunlockstatuses([str(cross_index)])[0], {'index': cross_index, 'status': 'mempooled', 'instantlock': True})
        tip_hash = self.generate(node, 1)[0]
        assert cross_txid in node_wallet.getblock(tip_hash)['tx']

        self.log.info("A rejected instance does not blacklist the txid shared by every instance of its withdrawal")
        # The node does not hold this withdrawal yet. An instance with a tampered requestedHeight
        # fails validation but carries the txid of the valid one, which a child already spends.
        orphan_index = 804
        unlock_valid = self.create_assetunlock(orphan_index, COIN, pubkey, version=2)
        parent_txid = self.get_v2_txid(unlock_valid)
        tampered_payload = CAssetUnlockTx()
        tampered_payload.deserialize(BytesIO(unlock_valid.vExtraPayload))
        tampered_payload.requestedHeight -= 1
        unlock_tampered = copy.deepcopy(unlock_valid)
        unlock_tampered.vExtraPayload = tampered_payload.serialize()
        assert_equal(self.get_v2_txid(unlock_tampered), parent_txid)
        assert unlock_tampered.rehash() != unlock_valid.rehash()

        parent_value = unlock_valid.vout[0].nValue
        orphan_child_hex = node_wallet.createrawtransaction(
            [{'txid': parent_txid, 'vout': 0}],
            {node_wallet.getnewaddress(): Decimal(parent_value - tiny_amount) / COIN})
        parent_prevout = {'txid': parent_txid, 'vout': 0, 'scriptPubKey': unlock_valid.vout[0].scriptPubKey.hex(),
                          'amount': Decimal(parent_value) / COIN}
        signed_orphan_child = node_wallet.signrawtransactionwithwallet(orphan_child_hex, [parent_prevout])
        assert signed_orphan_child['complete']
        orphan_child = tx_from_hex(signed_orphan_child['hex'])
        orphan_child_txid = orphan_child.rehash()

        relay_peer = node_wallet.add_p2p_connection(P2PInterface())
        with node_wallet.assert_debug_log([f"{parent_txid} from peer=", "was not accepted"]):
            relay_peer.send_and_ping(msg_tx(unlock_tampered))
        assert parent_txid not in node_wallet.getrawmempool()

        self.log.info("A child spending the withdrawal is kept as an orphan and accepted with its valid parent")
        with node_wallet.assert_debug_log(expected_msgs=[], unexpected_msgs=["not keeping orphan with rejected parents"]):
            relay_peer.send_and_ping(msg_tx(orphan_child))
        assert orphan_child_txid not in node_wallet.getrawmempool()
        relay_peer.send_and_ping(msg_tx(unlock_valid))
        mempool = node_wallet.getrawmempool()
        assert parent_txid in mempool
        assert orphan_child_txid in mempool
        self.wait_for_instantlock(parent_txid, orphan_child_txid)
        tip_hash = self.generate(node, 1)[0]
        mined_txids = node_wallet.getblock(tip_hash)['tx']
        assert parent_txid in mined_txids
        assert orphan_child_txid in mined_txids

        self.log.info("A peer relaying an older instance of an unlocked withdrawal does not keep it from being locked later")
        # A pending withdrawal over the credit pool limit keeps this one from being locked until it
        # expires; the per-block retry must then lock it without another re-sign
        tip_height = node_wallet.getblockcount()
        blocker = self.create_assetunlock(805, 4001 * COIN, pubkey, version=1,
                                          requested_height=tip_height - HEIGHT_DIFF_EXPIRING + 2)
        blocker_txid = self.send_tx_simple(blocker)
        unlock_old = self.create_assetunlock(806, COIN, pubkey, version=2)
        retry_lock_txid = self.get_v2_txid(unlock_old)
        assert_equal(self.send_tx_simple(unlock_old), retry_lock_txid)
        self.sync_mempools()
        # Empty blocks keep the withdrawal unmined while the blocker is pending
        self.generateblock(node, node_wallet.getnewaddress(), [])
        unlock_new = self.create_assetunlock(806, COIN, pubkey, version=2)
        assert_equal(self.send_tx_simple(unlock_new), retry_lock_txid)
        self.sync_unlock_instance(retry_lock_txid, unlock_new.rehash())
        assert blocker_txid in node_wallet.getrawmempool()
        assert_equal(node_wallet.getrawtransaction(retry_lock_txid, 1)['instantlock'], False)

        for mn in self.mninfo:
            mn_node = mn.get_node(self)
            with mn_node.assert_debug_log([f"{retry_lock_txid} from peer=", "assetunlock-stale-instance"]):
                mn_node.add_p2p_connection(P2PInterface()).send_and_ping(msg_tx(unlock_old))
            mn_node.disconnect_p2ps()

        self.log.info("Once the blocking withdrawal expires, the per-block retry locks the unlock")
        for _ in range(3):
            if blocker_txid not in node_wallet.getrawmempool():
                break
            self.generateblock(node, node_wallet.getnewaddress(), [])
        assert blocker_txid not in node_wallet.getrawmempool()
        assert retry_lock_txid in node_wallet.getrawmempool()
        self.wait_for_instantlock(retry_lock_txid)
        tip_hash = self.generate(node, 1)[0]
        assert retry_lock_txid in node_wallet.getblock(tip_hash)['tx']

        node_wallet.disconnect_p2ps()
        self.set_sporks()
        self.mempool_size = node_wallet.getmempoolinfo()['size']
        self.check_mempool_size()

    def test_non_standard(self, node_wallet, node, pubkey):
        self.log.info("Testing that v2 and >100-input asset locks are non-standard...")
        assert softfork_active(node_wallet, 'v24')

        coin = node_wallet.listunspent(query_options={'minimumAmount': 1}).pop()
        lock_v2 = self.create_assetlock(coin, COIN, pubkey, version=2)
        # reserve this coin so funding the split below can not spend it
        node_wallet.lockunspent(False, [{'txid': coin['txid'], 'vout': coin['vout']}])

        self.log.info("Split one coin into 101 outputs to build an asset lock with >100 inputs")
        raw = node_wallet.createrawtransaction([], [{node_wallet.getnewaddress(): 1} for _ in range(101)])
        funded = node_wallet.fundrawtransaction(raw, {'change_position': 101})['hex']
        split_txid = node_wallet.sendrawtransaction(node_wallet.signrawtransactionwithwallet(funded)['hex'])
        self.generate(node, 1)
        many_coins = [{'txid': split_txid, 'vout': i, 'amount': 1} for i in range(101)]
        tx_many_inputs = self.create_assetlock(many_coins, COIN, pubkey)
        assert_equal(len(tx_many_inputs.vin), 101)

        self.log.info("A standard node (-acceptnonstdtxn=0) rejects them; the permissive node accepts them")
        self.restart_node(1, self.extra_args[1] + ["-acceptnonstdtxn=0"])
        self.connect_nodes(1, 0)


        for tx, reason in [(lock_v2, 'assetlocktx-version-2'), (tx_many_inputs, 'assetlocktx-too-many-inputs')]:
            tx_hex = tx.serialize().hex()
            assert_equal(node_wallet.testmempoolaccept([tx_hex])[0]['allowed'], True)
            rejected = node.testmempoolaccept([tx_hex])[0]
            assert_equal(rejected['allowed'], False)
            assert_equal(rejected['reject-reason'], reason)

        self.log.info("They are still valid in a block: mine both and check the standard node accepts it")
        txids = [node_wallet.sendrawtransaction(tx.serialize().hex()) for tx in [lock_v2, tx_many_inputs]]
        block_hash = self.generate(node_wallet, 1)[0]
        for checked_node in self.nodes:
            for txid in txids:
                assert txid in checked_node.getblock(block_hash)['tx']

        self.log.info("The wallet refuses to build a v2 asset lock the local mempool would reject")
        self.restart_node(0, self.extra_args[0] + ["-acceptnonstdtxn=0"])
        assert_raises_rpc_error(-6, "assetlocktx-version-2", node_wallet.sendtoaddress,
                                encode_platform_p2pkh('tdash', hash160(pubkey)), 1)
        self.restart_node(0, self.extra_args[0])

        self.restart_node(1, self.extra_args[1])
        self.connect_nodes(1, 0)

    def test_replay_after_crash(self, node_wallet, node, pubkey):
        self.log.info("Testing recovery from a crash mid-flush with a quorum commitment and an asset unlock to replay...")
        # Every flush now dies after its first partial write, so nothing mined from here on is flushed
        self.restart_with_crashing_flush()

        self.mine_quorum_2_nodes()
        quorum_hash = node_wallet.quorum("list", 1)["llmq_test_platform"][0]
        commitment_block = node_wallet.quorum("info", llmq_type_test, quorum_hash)["minedBlock"]
        txid = self.send_tx(self.create_assetunlock(900, COIN, pubkey))
        unlock_block = self.generate(node_wallet, 1)[0]
        assert txid in node_wallet.getblock(unlock_block)["tx"]

        self.log.info("Restart without peers: the commitment block is replayed before the asset unlock block")
        self.crash_and_replay(node_wallet, node, [commitment_block, unlock_block])
        assert quorum_hash in node_wallet.quorum("list")["llmq_test_platform"]

        self.log.info("Replay an asset unlock as the first block rolled forward, with the quorum already mature and flushed")
        self.restart_with_crashing_flush()
        txid = self.send_tx(self.create_assetunlock(901, COIN, pubkey))
        unlock_block = self.generate(node_wallet, 1)[0]
        assert txid in node_wallet.getblock(unlock_block)["tx"]
        self.crash_and_replay(node_wallet, node, [unlock_block])

        self.restart_node(0, self.extra_args[0])
        for i in range(1, len(self.nodes)):
            self.connect_nodes(0, i)

    def restart_with_crashing_flush(self):
        self.restart_node(0, self.extra_args[0] + ["-dbcrashratio=1", "-dbbatchsize=1"])
        for i in range(1, len(self.nodes)):
            self.connect_nodes(0, i)

    def crash_and_replay(self, node_wallet, node, replayed_blocks):
        tip = node_wallet.getbestblockhash()
        self.log.info("Force a flush, which crashes the node")
        assert_raises((ConnectionError, http.client.CannotSendRequest, subprocess.CalledProcessError), node_wallet.gettxoutsetinfo)
        self.wait_for_node_exit(0, timeout=10)
        with node_wallet.assert_debug_log([f"Rolling forward {block}" for block in replayed_blocks]):
            self.start_node(0, extra_args=self.extra_args[0] + ["-connect=0"])
        assert_equal(node_wallet.getbestblockhash(), tip)
        assert_equal(node_wallet.gettxoutsetinfo()["hash_serialized_3"], node.gettxoutsetinfo()["hash_serialized_3"])

    def sync_unlock_instance(self, txid, instance_hash):
        # A re-signed instance shares the held entry's txid, so sync_mempools() is satisfied
        # before the refresh has propagated; wait for every node to hold this exact instance.
        # Refreshes travel as MSG_ASSET_UNLOCK invs on the trickle schedule, which needs time
        # to advance.
        def synced():
            self.bump_mocktime(1)
            return all(node.getrawtransaction(txid, 1).get('instanceHash') == instance_hash
                       for node in self.nodes)
        self.wait_until(synced, timeout=60)


if __name__ == '__main__':
    AssetLocksV24Test().main()
