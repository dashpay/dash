#!/usr/bin/env python3
# Copyright (c) 2018-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import random
from decimal import Decimal
from io import BytesIO

from test_framework.messages import (
    COIN,
    COutPoint,
    CRecoveredSig,
    hash256,
    msg_isdlock,
    msg_qsendrecsigs,
    msg_qsigrec,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    create_lots_of_big_transactions,
    gen_return_txouts,
)
from test_framework.wallet import MiniWallet

'''
p2p_instantsend.py

Tests InstantSend functionality (prevent doublespend for unconfirmed transactions)
'''

class RecSigsObserver(P2PInterface):
    """Non-MN peer that opts in to recsigs and records every ISDLOCK inv it sees."""

    def __init__(self):
        super().__init__()
        self.isdlock_inv_seen = False

    def send_qsendrecsigs(self, wants_recsigs=True):
        self.send_message(msg_qsendrecsigs(wants_recsigs))

    def on_inv(self, message):
        for inv in message.inv:
            # MSG_ISDLOCK inv type, see src/protocol.h
            if inv.type == 31:
                self.isdlock_inv_seen = True
        super().on_inv(message)


class InstantSendTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # One masternode with single-member quorums: this test is about InstantSend on the non-masternode
        # nodes, not about the quorums themselves
        self.set_dash_test_params(5, 1, [["-llmqtestinstantsenddip0024=llmq_test_instantsend"]] * 5)
        self.set_dash_llmq_test_params(1, 1)
        # set sender,  receiver,  isolated nodes
        self.isolated_idx = 1
        self.receiver_idx = 2
        self.sender_idx = 3
        # The isolated node runs the smallest mempool allowed, and relays the large transactions
        # test_full_mempool_keeps_locked_txs fills it with
        self.extra_args[self.isolated_idx] += ["-maxmempool=5", "-datacarriersize=100000"]
        # The masternode's small mempool makes it stop locking transactions sooner
        self.extra_args[-1] += ["-maxmempool=5"]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.log.info("Mine quorums for InstantSend and ChainLocks")
        self.mine_quorum_single_member()

        self.test_mempool_doublespend()
        self.test_block_doublespend()
        self.test_isdlock_relayed_to_recsigs_observer()
        self.test_genesis_cycle_islock_isolated()
        self.test_cached_recsig_islock_isolated()
        self.test_provider_tx_not_locked()
        self.test_full_mempool_keeps_locked_txs()

    def test_block_doublespend(self):
        sender = self.nodes[self.sender_idx]
        receiver = self.nodes[self.receiver_idx]
        isolated = self.nodes[self.isolated_idx]

        # feed the sender with some balance
        sender_addr = sender.getnewaddress()
        is_id = self.nodes[0].sendtoaddress(sender_addr, 1)
        self.wait_for_instantlock(is_id)
        self.generate(self.nodes[0], 2)

        # create doublespending transaction, but don't relay it
        dblspnd_tx = self.create_raw_tx(sender, isolated, 0.5, 1, 100)
        # isolate one node from network
        self.isolate_node(self.isolated_idx)
        # instantsend to receiver
        receiver_addr = receiver.getnewaddress()
        is_id = sender.sendtoaddress(receiver_addr, 0.9)
        # wait for the transaction to propagate
        connected_nodes = self.nodes.copy()
        del connected_nodes[self.isolated_idx]
        self.wait_for_instantlock(is_id, nodes=connected_nodes)
        # send doublespend transaction to isolated node, whose clock missed the bumps made while waiting for the lock
        isolated.setmocktime(self.mocktime)
        dblspnd_txid = isolated.sendrawtransaction(dblspnd_tx['hex'])
        # a transaction without a lock is not mined until it is 10 minutes old
        self.bump_mocktime(599)
        early_block = self.generate(isolated, 1, sync_fun=self.no_op)[0]
        assert dblspnd_txid not in isolated.getblock(early_block)["tx"]
        assert dblspnd_txid in isolated.getrawmempool()
        isolated.invalidateblock(early_block)
        # generate block on isolated node with doublespend transaction
        self.bump_mocktime(1)
        wrong_block = self.generate(isolated, 1, sync_fun=self.no_op)[0]
        assert_equal(isolated.getrawtransaction(dblspnd_txid, 1)["confirmations"], 1)
        # connect isolated block to network
        self.reconnect_isolated_node(self.isolated_idx, 0)
        # check doublespend block is rejected
        self.wait_until(lambda: any(tip["hash"] == wrong_block and tip["status"] == "invalid"
                                    for tip in self.nodes[0].getchaintips()))
        self.sync_blocks([n for n in self.nodes if n is not isolated])
        for node in self.nodes:
            if node is not isolated:
                assert node.getbestblockhash() != wrong_block
        # send coins back to the controller node without waiting for confirmations
        receiver.sendtoaddress(self.nodes[0].getnewaddress(), 0.9, "", "", True)
        assert_equal(receiver.getwalletinfo()["balance"], 0)
        # mine more blocks
        # TODO: mine these blocks on an isolated node
        self.bump_mocktime(1)
        # make sure the above TX is on node0
        self.sync_mempools([n for n in self.nodes if n is not isolated])
        self.generate(self.nodes[0], 2)

    def test_mempool_doublespend(self):
        sender = self.nodes[self.sender_idx]
        receiver = self.nodes[self.receiver_idx]
        isolated = self.nodes[self.isolated_idx]
        connected_nodes = self.nodes.copy()
        del connected_nodes[self.isolated_idx]

        # feed the sender with some balance
        sender_addr = sender.getnewaddress()
        is_id = self.nodes[0].sendtoaddress(sender_addr, 1)
        self.wait_for_instantlock(is_id)
        self.generate(self.nodes[0], 2)

        # create doublespending transaction, but don't relay it
        dblspnd_tx = self.create_raw_tx(sender, isolated, 0.5, 1, 100)
        # isolate one node from network
        self.isolate_node(self.isolated_idx)
        # send doublespend transaction to isolated node
        dblspnd_txid = isolated.sendrawtransaction(dblspnd_tx['hex'])
        assert dblspnd_txid in set(isolated.getrawmempool())
        # let isolated node rejoin the network
        # The previously isolated node should NOT relay the doublespending TX
        self.reconnect_isolated_node(self.isolated_idx, 0)
        for node in connected_nodes:
            assert_raises_rpc_error(-5, "No such mempool or blockchain transaction", node.getrawtransaction, dblspnd_txid)
        # Instantsend to receiver. The previously isolated node won't accept the tx but it should
        # request the correct TX from other nodes once the corresponding lock is received.
        # And this time the doublespend TX should be pruned once the correct tx is received.
        receiver_addr = receiver.getnewaddress()
        is_id = sender.sendtoaddress(receiver_addr, 0.9)
        # wait for the transaction to propagate
        self.wait_for_instantlock(is_id)
        assert dblspnd_txid not in set(isolated.getrawmempool())
        # send coins back to the controller node without waiting for confirmations
        sentback_id = receiver.sendtoaddress(self.nodes[0].getnewaddress(), 0.9, "", "", True)
        self.wait_for_instantlock(sentback_id)
        assert_equal(receiver.getwalletinfo()["balance"], 0)
        # mine more blocks
        self.generate(self.nodes[0], 2)

    def test_isdlock_relayed_to_recsigs_observer(self):
        self.log.info("Non-MN peer started with -watchquorums must still get ISDLOCK invs")
        observers = []
        for mn in self.mninfo:
            node = mn.get_node(self)
            obs = node.add_p2p_connection(RecSigsObserver())
            obs.send_qsendrecsigs(True)
            obs.sync_with_ping()
            observers.append((node, obs))

        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        self.wait_for_instantlock(txid)

        for _, obs in observers:
            obs.wait_until(lambda obs=obs: obs.isdlock_inv_seen, timeout=10)

        for node, _ in observers:
            node.disconnect_p2ps()

    def test_genesis_cycle_islock_isolated(self):
        self.log.info("A missing-quorum ISDLOCK must not drop the genuine locks batched with it")
        # A genesis-block cycle hash resolves to height 0, a valid rotation boundary, but selects a
        # signing height from before any quorum, so the lock has no quorum. Before the fix that
        # dropped the whole verification batch, taking any genuine lock that shared it. We isolate a
        # node so its only lock source is our peer, then queue a genuine lock alongside one
        # quorum-less lock.
        controller = self.nodes[0]
        target = self.nodes[self.isolated_idx]
        connected = [n for i, n in enumerate(self.nodes) if i != self.isolated_idx]
        self.isolate_node(self.isolated_idx)
        target.setnetworkactive(True)
        peer = target.add_p2p_connection(P2PInterface())
        genesis_hash = int(target.getblockhash(0), 16)

        def genesis_lock(sig):
            # No quorum signs at height 0; reusing a real signature keeps sig.IsValid() true so the
            # lock still reaches (and fails) quorum selection instead of being rejected as malformed.
            return msg_isdlock(1, [COutPoint(random.getrandbits(256), 0)], random.getrandbits(256), genesis_hash, sig)

        txid = controller.sendtoaddress(controller.getnewaddress(), 1)
        self.wait_for_instantlock(txid, nodes=connected)
        genuine = msg_isdlock()
        genuine.deserialize(BytesIO(bytes.fromhex(controller.getislocks([txid])[0]["hex"])))
        target.sendrawtransaction(controller.getrawtransaction(txid))
        # Hold the worker so both locks are queued before it fetches them as a single batch.
        target.setinstantsendworkeractive(False)
        peer.send_message(genesis_lock(genuine.sig))
        peer.send_message(genuine)
        peer.sync_with_ping()
        # The quorum-less lock is retried against the previous active set after the genuine one is applied.
        with target.assert_debug_log(["doing verification on old active set", "verified locks. count=0"], timeout=20):
            target.setinstantsendworkeractive(True)
            self.wait_until(lambda: target.getrawtransaction(txid, True)["instantlock"], timeout=20)
        peer.sync_with_ping()
        assert peer.is_connected

        target.disconnect_p2ps()
        self.reconnect_isolated_node(self.isolated_idx, 0)

    def test_cached_recsig_islock_isolated(self):
        self.log.info("A cached recovered sig must not authenticate another ISDLOCK for the same transaction")
        llmq_type = 104  # llmq_test_instantsend
        controller = self.nodes[0]
        target = self.nodes[self.isolated_idx]
        connected = [n for i, n in enumerate(self.nodes) if i != self.isolated_idx]
        self.isolate_node(self.isolated_idx)
        target.setnetworkactive(True)
        forger = target.add_p2p_connection(P2PInterface())
        honest = target.add_p2p_connection(P2PInterface())

        locks = []
        for _ in range(2):
            txid = controller.sendtoaddress(controller.getnewaddress(), 1)
            self.wait_for_instantlock(txid, nodes=connected)
            islock_json = controller.getislocks([txid])[0]
            islock = msg_isdlock()
            islock.deserialize(BytesIO(bytes.fromhex(islock_json["hex"])))
            locks.append((txid, islock, islock_json["id"]))
            # The target must have both transactions, a reconnect does not relay them again
            target.sendrawtransaction(controller.getrawtransaction(txid))
        (txid, genuine, request_id), (_, other, _) = locks

        # The target learns the recovered sig of the lock, but not the lock itself
        rec_sig_json = self.mninfo[0].get_node(self).quorum("getrecsig", llmq_type, request_id, txid)
        rec_sig = CRecoveredSig()
        rec_sig.llmqType = rec_sig_json["llmqType"]
        rec_sig.quorumHash = int(rec_sig_json["quorumHash"], 16)
        rec_sig.id = int(rec_sig_json["id"], 16)
        rec_sig.msgHash = int(rec_sig_json["msgHash"], 16)
        rec_sig.sig = bytes.fromhex(rec_sig_json["sig"])
        honest.send_message(msg_qsigrec(rec_sig))
        self.wait_until(lambda: target.quorum("hasrecsig", llmq_type, request_id, txid))

        # Same transaction, but with a signature that belongs to another lock
        forged = msg_isdlock(genuine.nVersion, genuine.inputs, genuine.txid, genuine.cycleHash, other.sig)
        forged_hash = hash256(forged.serialize())[::-1].hex()
        with target.assert_debug_log([f"txid={txid}, islock={forged_hash}: verification failed"], timeout=20):
            forger.send_message(forged)
        assert_equal(target.getislocks([txid]), ["None"])

        honest.send_message(genuine)
        self.wait_until(lambda: target.getrawtransaction(txid, True)["instantlock"], timeout=20)
        assert_equal(target.getislocks([txid])[0]["hex"], genuine.serialize().hex())

        target.disconnect_p2ps()
        self.reconnect_isolated_node(self.isolated_idx, 0)

    def test_provider_tx_not_locked(self):
        self.log.info("Provider txs are not locked, but are mined once safe and their block is ChainLocked")
        node = self.nodes[0]
        funds_addr = node.getnewaddress()
        fund_id = node.sendtoaddress(funds_addr, 1)
        self.wait_for_instantlock(fund_id)

        protx_hex = self.mninfo[0].update_service(node, submit=False, fundsAddr=funds_addr)
        protx_id = node.decoderawtransaction(protx_hex)["txid"]
        mn = self.mninfo[0].get_node(self)
        with mn.assert_debug_log([
            f"CheckCanLock -- txid={protx_id}: provider txs are not locked",
            f"ProcessTx -- txid={protx_id}: CheckCanLock returned false",
        ], timeout=60):
            node.sendrawtransaction(protx_hex)
            self.bump_mocktime(30)
            self.sync_mempools()
        later_id = node.sendtoaddress(node.getnewaddress(), 1)
        self.wait_for_instantlock(later_id)
        for n in self.nodes:
            assert not n.getrawtransaction(protx_id, True)["instantlock_internal"]

        tip = self.bury_tx(node, protx_id)
        self.wait_for_chainlocked_block(node, tip)

        self.log.info("Provider txs first seen in a block are not locked retroactively")
        fund_id = node.sendtoaddress(funds_addr, 1)
        self.wait_for_instantlock(fund_id)
        self.isolate_node(0)
        protx_id = self.mninfo[0].update_service(node, submit=True, fundsAddr=funds_addr)
        normal_id = node.sendtoaddress(node.getnewaddress(), 1)
        self.bump_mocktime(10 * 60 + 1)
        block_hash = self.generate(node, 1, sync_fun=self.no_op)[0]
        block_txs = node.getblock(block_hash)["tx"]
        assert protx_id in block_txs
        assert normal_id in block_txs

        with mn.assert_debug_log([
            f"CheckCanLock -- txid={protx_id}: provider txs are not locked",
            f"ProcessTx -- txid={protx_id}: CheckCanLock returned false",
        ], timeout=60):
            node.setnetworkactive(True)
            for i in range(1, self.num_nodes):
                self.connect_nodes(0, i)
            self.sync_blocks()
        self.wait_until(lambda: all(n.getrawtransaction(normal_id, True)["instantlock_internal"] for n in self.nodes))
        for n in self.nodes:
            assert not n.getrawtransaction(protx_id, True)["instantlock_internal"]
            assert not n.getblock(block_hash)["chainlock"]

        self.log.info("Children of a provider tx are locked once its block is ChainLocked")
        protx_out = node.getrawtransaction(protx_id, True)["vout"][0]
        child_raw = node.createrawtransaction([{"txid": protx_id, "vout": 0}], {node.getnewaddress(): protx_out["value"] - Decimal("0.001")})
        child_hex = node.signrawtransactionwithwallet(child_raw)["hex"]
        child_id = node.decoderawtransaction(child_hex)["txid"]
        with mn.assert_debug_log([
            f"CheckCanLock -- txid={child_id}: outpoint {protx_id}-0 too new and not ChainLocked",
        ], timeout=60):
            node.sendrawtransaction(child_hex)
            self.bump_mocktime(30)
            self.sync_mempools()
        for n in self.nodes:
            assert not n.getrawtransaction(child_id, True)["instantlock_internal"]

        self.bump_mocktime(10 * 60 + 1)
        self.wait_for_chainlocked_block_all_nodes(block_hash)
        self.wait_until(lambda: all(n.getrawtransaction(child_id, True)["instantlock_internal"] for n in self.nodes))
        assert_equal(node.getbestblockhash(), block_hash)

    def test_full_mempool_keeps_locked_txs(self):
        self.log.info("A full mempool keeps locked transactions and their locks")
        sender = self.nodes[self.sender_idx]
        receiver = self.nodes[self.receiver_idx]
        small = self.nodes[self.isolated_idx]
        others = [n for n in self.nodes if n is not small]
        relayfee = small.getnetworkinfo()["relayfee"]

        # Large transactions only the small node accepts: nobody else relays or locks them
        num_big_txs = 80
        num_locked_txs = 40
        wallet = MiniWallet(small)
        fund_id = self.nodes[0].sendtoaddress(wallet.get_address(), 100)
        self.wait_for_instantlock(fund_id)
        self.generate(self.nodes[0], 1)
        wallet.rescan_utxos()
        split_id = wallet.send_self_transfer_multi(from_node=small, num_outputs=num_big_txs + num_locked_txs)["txid"]
        self.wait_for_instantlock(split_id)

        sender_addr = sender.getnewaddress()
        is_id = self.nodes[0].sendtoaddress(sender_addr, 1)
        self.wait_for_instantlock(is_id)
        self.generate(self.nodes[0], 2)

        dblspnd_tx = self.create_raw_tx(sender, sender, 0.5, 1, 100)
        locked_id = sender.sendtoaddress(receiver.getnewaddress(), 0.9, fee_rate=1)
        self.wait_for_instantlock(locked_id)

        self.log.info("Fill the small mempool with transactions paying more than the locked one")
        txouts = gen_return_txouts()
        for i in range(num_big_txs):
            # each is about 65 kB, so this starts at twice the minimum relay feerate
            create_lots_of_big_transactions(wallet, small, (i + 1) * 130 * relayfee, 1, txouts)
        assert_greater_than(small.getmempoolinfo()["mempoolminfee"], relayfee)

        assert locked_id in small.getrawmempool()
        assert small.getrawtransaction(locked_id, True)["instantlock_internal"]
        assert_raises_rpc_error(-26, "tx-txlock-conflict", small.sendrawtransaction, dblspnd_tx["hex"])

        self.log.info("A locked transaction is accepted below the fee floor of a full mempool")
        late_id = sender.sendtoaddress(receiver.getnewaddress(), 0.05, fee_rate=1)
        self.wait_for_instantlock(late_id, nodes=others)

        def late_tx_arrived():
            self.bump_mocktime(1)
            return late_id in small.getrawmempool()
        self.wait_until(late_tx_arrived, sleep=0.5)
        self.wait_until(lambda: small.getrawtransaction(late_id, True)["instantlock_internal"])
        assert_greater_than(small.getmempoolinfo()["mempoolminfee"], relayfee)

        self.log.info("Both are mined, and the small node mines away what only it holds")
        tip = self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks)[0]
        block_txs = self.nodes[0].getblock(tip)["tx"]
        assert locked_id in block_txs and late_id in block_txs
        self.bump_mocktime(10 * 60 + 1)
        while small.getrawmempool():
            self.generate(small, 1, sync_fun=self.sync_blocks)
        self.sync_mempools()

        self.log.info("Once locked transactions take up half of a masternode's mempool, it leaves the rest to be mined")
        mn = self.mninfo[0].get_node(self)
        log_start = mn.debug_log_path.stat().st_size

        def not_signed(txid):
            with open(mn.debug_log_path, encoding="utf-8", errors="replace") as log:
                log.seek(log_start)
                return f"txid={txid}: not signing, locked transactions take up half of the mempool" in log.read()

        locked_ids = []
        while True:
            # about 100 kB each, the largest transaction relayed by default
            txid = wallet.send_self_transfer_multi(from_node=mn, num_outputs=2700)["txid"]
            self.wait_until(lambda: mn.getrawtransaction(txid, True)["instantlock_internal"] or not_signed(txid))
            if not_signed(txid):
                break
            locked_ids.append(txid)
        assert_greater_than(len(locked_ids), 0)
        self.bump_mocktime(30)
        self.sync_mempools()
        for node in self.nodes:
            assert not node.getrawtransaction(txid, True)["instantlock_internal"]

        self.log.info("It is locked once it is mined, and transactions are locked in the mempool again")
        # Without ChainLocks, or its block could be ChainLocked before the masternode gets to lock it
        self.nodes[0].sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 4000000000)
        self.wait_for_sporks_same()
        # Mined on its own: the locked transactions stay in the masternode's mempool meanwhile
        for locked_id in locked_ids:
            self.nodes[0].prioritisetransaction(locked_id, -COIN)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(self.nodes[0], 1)
        self.wait_until(lambda: all(node.getrawtransaction(txid, True)["instantlock_internal"] for node in self.nodes))
        assert_equal(sorted(mn.getrawmempool()), sorted(locked_ids))
        for locked_id in locked_ids:
            self.nodes[0].prioritisetransaction(locked_id, COIN)
        self.nodes[0].sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 0)
        self.wait_for_sporks_same()
        while self.nodes[0].getrawmempool():
            tip = self.generate(self.nodes[0], 1)[0]
        self.wait_for_chainlocked_block_all_nodes(tip)
        resumed_id = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        self.wait_for_instantlock(resumed_id)


if __name__ == '__main__':
    InstantSendTest().main()
