#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Check assumeutxo end to end on a masternode network.

On a chain with rotating quorums, the evo section dumptxoutset appends to a
UTXO snapshot carries the rotation quarters the planner derives from the
carried commitments and the base's cycle, the work blocks of the next cycle
when they are already mined, the credit-pool window and the header-proven
base coinbase. Its hash is the single SHA256 of the section bytes in the file.

Three cold nodes, which know only the headers, then load node 0's dumps at
three positions in the rotation cycle, one of them inside the mining window:

- node X rejects mutated files and pins, then loads; -reindex and
  -reindex-chainstate drop its snapshot, after which it syncs from blocks;
- node R loads an evo section that differs from the validated state in a
  field only completion checks, under a pin reviewed against that section.
  Completion rejects it without blaming a block or a peer, and after the
  restart the node syncs from blocks and refuses another snapshot while the
  rejected one is kept;
- node A syncs the snapshot chain through InstantSend, asset-unlock and EHF
  traffic, an InstantSend conflict and a ChainLock while its background
  chainstate is still before v19, stalls on an EHF signal of a quorum the
  snapshot does not carry, completes background validation in the same run,
  refuses to start as a masternode, and connects that block once a restart
  has promoted the snapshot.
"""

import hashlib
from decimal import Decimal
from pathlib import Path
import struct

from test_framework.key import ECKey
from test_framework.messages import (
    CAssetLockTx,
    CAssetUnlockTx,
    CBlock,
    CBlockHeader,
    CInv,
    CMnEhf,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    from_hex,
    hash256,
    msg_block,
    msg_clsig,
    msg_headers,
    msg_inv,
    msg_isdlock,
    ser_string,
    uint256_from_str,
)
from test_framework.p2p import P2PInterface
from test_framework.script import CScript, OP_RETURN
from test_framework.script_util import key_to_p2pk_script, key_to_p2pkh_script
from test_framework.test_framework import DashTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    force_finish_mnsync,
    initialize_datadir,
)
from test_framework.wallet_util import bytes_to_wif

EVO_MARKER = b"DASHEVO\x00"
LLMQ_TEST = 100
LLMQ_TEST_DIP0024 = 103
LLMQ_TEST_PLATFORM = 106
CYCLE = 24
CREDIT_POOL_PERIOD = 100  # regtest
V19_HEIGHT = 60
# Bits no deployment uses, so masternodes never sign them on their own.
OLD_QUORUM_EHF_BIT = 27
CARRIED_QUORUM_EHF_BIT = 25
TINY_AMOUNT = int(Decimal("0.0007") * COIN)
WITHDRAWAL_INDEX = 101
SPORK_KEY = "cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"
MASTERNODE_REFUSAL = "An assumeutxo snapshot chainstate is in use, which a masternode cannot run on."
COMMON_ARGS = [f"-testactivationheight=v19@{V19_HEIGHT}"]


class LockRelay(P2PInterface):
    """Announces ChainLocks and InstantSend locks, and serves them on request."""
    def __init__(self):
        super().__init__()
        self.objects = {}

    def announce(self, msg, inv_type):
        h = uint256_from_str(hash256(msg.serialize()))
        self.objects[h] = msg
        self.send_message(msg_inv([CInv(inv_type, h)]))

    def on_getdata(self, message):
        for inv in message.inv:
            if inv.hash in self.objects:
                self.send_message(self.objects[inv.hash])


class BlockServer(P2PInterface):
    """Announces the given blocks' headers and serves only those blocks."""
    def __init__(self, blocks):
        super().__init__()
        self.blocks = {b.sha256: b for b in blocks}
        self.headers = [CBlockHeader(b) for b in blocks]

    def on_getdata(self, message):
        for inv in message.inv:
            if inv.hash in self.blocks:
                self.send_message(msg_block(self.blocks[inv.hash]))


class AssumeutxoDashTest(DashTestFramework):
    def set_test_params(self):
        args = COMMON_ARGS + ["-llmqtestparams=4:4", "-llmqtestinstantsendparams=4:4", "-llmqtestplatformparams=2:2"]
        self.set_dash_test_params(5, 4, extra_args=[list(args) for _ in range(5)], evo_count=2)
        self.llmq_size = 4
        self.llmq_threshold = 4
        self.rpc_timeout = 120

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.importer_args = {}
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.key = ECKey()
        self.key.generate()
        node.importprivkey(bytes_to_wif(self.key.get_bytes()))
        # Platform quorums, which sign withdrawals, have only EvoNodes as members.
        for _ in range(2):
            self.dynamically_add_masternode(evo=True)

        first = self.cycle_of(self.mine_cycle_quorum()[0]["height"])
        # Signed now, mined only above node A's base, by when its quorum is
        # older than every quorum the snapshot carries.
        self.old_ehf, self.old_ehf_quorum = self.create_mnehf(OLD_QUORUM_EHF_BIT)
        self.lock_credits()
        second = self.cycle_of(self.mine_cycle_quorum()[0]["height"])
        assert_equal(second, first + CYCLE)

        self.log.info("Dump at a base before the mining window of its own cycle")
        h = second + CYCLE
        self.advance_to(h + 3)
        self.dump_x = self.dump("before-window.dat")
        rotating = self.quorum_type(self.dump_x["evo"], LLMQ_TEST_DIP0024)
        assert_equal(rotating["rotation"], True)
        assert_equal(rotating["active_commitments"], 2)
        assert_equal(rotating["safety_commitments"], 2)
        base = h + 3
        # A_T: the carried cycles, the base's cycle h and the next cycle N.
        expected = self.quarters({first, second, h, h + CYCLE}, base)
        assert_equal(set(rotating["rotation_cycles"]), expected)
        assert_equal(expected, {first - 3 * CYCLE, first - 2 * CYCLE, first - CYCLE, first, second, h})
        # Every cycle up to h has its quarter carried, and N's work block is
        # not mined yet.
        assert_equal(rotating["bootstrap_work"], [])

        self.log.info("Dump at the block before the next cycle")
        self.advance_to(h + CYCLE - 1)
        self.dump_r = self.dump("before-next-cycle.dat")
        rotating = self.quorum_type(self.dump_r["evo"], LLMQ_TEST_DIP0024)
        # N's work block is mined but no quorum commitment references it.
        assert_equal(rotating["bootstrap_work"], [h + CYCLE])
        # Mined just above the base, so that R's mempool holds a child of it.
        # Node 0 mines it only once it is locked, and its wallet keeps the
        # output unspent for R.
        address = node.getnewaddress()
        self.r_parent_txid = node.sendtoaddress(address, 1)
        self.r_parent_vout, = [o["n"] for o in node.getrawtransaction(self.r_parent_txid, True)["vout"]
                               if o["scriptPubKey"].get("address") == address]
        node.lockunspent(False, [{"txid": self.r_parent_txid, "vout": self.r_parent_vout}])
        self.wait_for_instantlock(self.r_parent_txid)

        # Carried by the last base, and the newest one of its type when it signs
        # a withdrawal above that base.
        self.mine_quorum(llmq_type_name="llmq_test_platform", llmq_type=LLMQ_TEST_PLATFORM, expected_members=2,
                         expected_connections=1, expected_contributions=2, expected_commitments=2)

        # Two active and two safety quorums of a type are carried, so four
        # newer ones leave the EHF signal's quorum out of the snapshot.
        while self.nodes[0].quorum("list", 10)["llmq_test"].index(self.old_ehf_quorum) < 4:
            self.mine_cycle_quorum()

        self.log.info("Dump at a base inside the mining window")
        self.advance_to(self.cycle_of(node.getblockcount()) + CYCLE + 18)
        self.dump_a = self.dump("in-window.dat")

        self.mine_traffic_above_base()

        self.test_mutated_snapshots_and_reindex()
        self.test_rejected_at_completion()
        self.test_dual_mode()

    # Chain building ---------------------------------------------------------

    def cycle_of(self, height):
        return height - height % CYCLE

    def advance_to(self, height):
        node = self.nodes[0]
        assert node.getblockcount() < height
        self.move_blocks(self.nodes, height - 1 - node.getblockcount())
        # The base certifies a recent ChainLock, so its ChainLock scans are
        # provable from the carried commitments.
        self.wait_for_chainlocked_block_all_nodes(node.getbestblockhash())
        self.move_blocks(self.nodes, 1)
        assert_equal(node.getblockcount(), height)

    def quarters(self, cycles, base):
        """S_T: the three quarters before each cycle that exist at the base."""
        return {c - k * CYCLE for c in cycles for k in (1, 2, 3) if CYCLE <= c - k * CYCLE <= base}

    def quorum_type(self, evo, llmq_type):
        matches = [q for q in evo["quorums"] if q["llmq_type"] == llmq_type]
        assert_equal(len(matches), 1)
        return matches[0]

    def dump(self, filename):
        node = self.nodes[0]
        result = node.dumptxoutset(filename)
        evo = result["evo"]
        assert_equal(result["base_height"], node.getblockcount())
        assert_equal(evo["version"], 4)
        assert_equal(evo["mn_count"], len(self.mninfo))
        assert_equal(evo["credit_history"], CREDIT_POOL_PERIOD)
        assert_equal(evo["base_coinbase"], True)
        assert evo["quorum_modifiers"] > 0
        assert evo["historical_mn_lists"] > 0

        # The section ends the file and its hash covers exactly its bytes.
        data = (node.chain_path / filename).read_bytes()
        start = data.find(EVO_MARKER)
        assert start > 0
        digest = hashlib.sha256(data[start + len(EVO_MARKER):]).digest()
        assert_equal(digest[::-1].hex(), evo["hash"])

        # Another node at the same tip, which derived its state itself, dumps
        # the same section.
        self.sync_blocks()
        assert_equal(self.nodes[1].dumptxoutset(filename)["evo"]["hash"], evo["hash"])
        return result

    def pin(self, dump, evo_hash=None, txoutset_hash=None):
        return (f"-assumeutxodata={dump['base_height']}:{txoutset_hash or dump['txoutset_hash']}:"
                f"{evo_hash or dump['evo']['hash']}:{dump['nchaintx']}:{dump['base_hash']}")

    def args_with_pin(self, idx, dump, **pin_overrides):
        """Node idx's arguments, with dump's base pinned as given instead."""
        kept = [a for a in self.importer_args[idx] if dump["base_hash"] not in a]
        return kept + [self.pin(dump, **pin_overrides)]

    @staticmethod
    def write_flipped(dump, offset, suffix=".mod"):
        """A copy of dump's file with the byte at offset flipped."""
        data = Path(dump["path"]).read_bytes()
        path = dump["path"] + suffix
        Path(path).write_bytes(data[:offset] + bytes([data[offset] ^ 1]) + data[offset + 1:])
        return path

    def create_mnehf(self, bit):
        request_id = hash256(ser_string(b"mnhf") + struct.pack("<Q", bit))[::-1].hex()
        quorum_hash = self.mninfo[0].get_node(self).quorum("selectquorum", LLMQ_TEST, request_id)["quorumHash"]
        payload = CMnEhf(version=1, versionBit=bit, quorumHash=int(quorum_hash, 16), quorumSig=b'\00' * 96)
        tx = CTransaction()
        tx.nVersion = 3
        tx.nType = 7
        tx.vExtraPayload = payload.serialize()
        tx.calc_sha256()
        payload.quorumSig = bytes.fromhex(self.get_recovered_sig(request_id, f"{tx.sha256:064x}")["sig"])
        tx.vExtraPayload = payload.serialize()
        return tx, quorum_hash

    def lock_credits(self):
        """Fund the credit pool, so a withdrawal can follow above a base."""
        node = self.nodes[0]
        coin = next(c for c in node.listunspent() if c["amount"] > 10)
        amount = 5 * COIN
        pubkey = self.key.get_pubkey().get_bytes()
        lock = CTransaction()
        lock.vin = [CTxIn(COutPoint(int(coin["txid"], 16), coin["vout"]))]
        change = int(coin["amount"] * COIN) - amount - TINY_AMOUNT
        lock.vout = [CTxOut(change, key_to_p2pk_script(pubkey)), CTxOut(amount, CScript([OP_RETURN, b""]))]
        lock.nVersion = 3
        lock.nType = 8
        lock.vExtraPayload = CAssetLockTx(1, [CTxOut(amount, key_to_p2pkh_script(pubkey))]).serialize()
        signed = node.signrawtransactionwithwallet(lock.serialize().hex())["hex"]
        node.sendrawtransaction(signed)
        self.generate(node, 1)

    def create_withdrawal(self, index, amount):
        node = self.nodes[0]
        request_id = hash256(ser_string(b"plwdtx") + struct.pack("<Q", index))[::-1].hex()
        quorum_hash = self.mninfo[0].get_node(self).quorum("selectquorum", LLMQ_TEST_PLATFORM, request_id)["quorumHash"]
        payload = CAssetUnlockTx(version=1, index=index, fee=TINY_AMOUNT, requestedHeight=node.getblockcount(),
                                 quorumHash=int(quorum_hash, 16), quorumSig=b'\00' * 96)
        tx = CTransaction()
        tx.vout = [CTxOut(amount - TINY_AMOUNT, key_to_p2pk_script(self.key.get_pubkey().get_bytes()))]
        tx.nVersion = 3
        tx.nType = 9
        tx.vExtraPayload = payload.serialize()
        tx.calc_sha256()
        recsig = self.get_recovered_sig(request_id, f"{tx.sha256:064x}", llmq_type=LLMQ_TEST_PLATFORM, use_platformsign=True)
        payload.quorumSig = bytes.fromhex(recsig["sig"])
        tx.vExtraPayload = payload.serialize()
        return tx, recsig, quorum_hash

    def islock_msg(self, txid):
        islock, = self.nodes[0].getislocks([txid])
        inputs = [COutPoint(int(i["txid"], 16), i["vout"]) for i in islock["inputs"]]
        return msg_isdlock(1, inputs, int(txid, 16), int(islock["cycleHash"], 16), bytes.fromhex(islock["signature"]))

    def chainlock_msg(self):
        cl = self.nodes[0].getbestchainlock()
        return cl, msg_clsig(cl["height"], int(cl["blockhash"], 16), bytes.fromhex(cl["signature"]))

    def mine_traffic_above_base(self):
        """Mine, above node A's base, what its snapshot chain must process."""
        node = self.nodes[0]
        self.log.info("Mine InstantSend, withdrawal, conflict and EHF traffic above the last base")
        # V24's EHF signal is mined below every base; this one is signed by a
        # quorum the last base carries.
        ehf, _ = self.create_mnehf(CARRIED_QUORUM_EHF_BIT)
        carried_ehf_txid = node.sendrawtransaction(ehf.serialize().hex(), maxfeerate=0)
        self.is_txid = node.sendtoaddress(node.getnewaddress(), 1)
        self.wait_for_instantlock(self.is_txid)
        self.generate(node, 1)
        assert carried_ehf_txid in node.getblock(node.getbestblockhash())["tx"]

        withdrawal, self.platform_recsig, self.platform_quorum = self.create_withdrawal(WITHDRAWAL_INDEX, COIN)
        withdrawal_txid = node.sendrawtransaction(withdrawal.serialize().hex())
        self.generate(node, 1)
        assert withdrawal_txid in node.getblock(node.getbestblockhash())["tx"]

        # An InstantSend lock signed in the next cycle, delivered to node A
        # only once a later cycle's quorums sign InstantSend.
        self.mine_cycle_quorum()
        cycle_txid = node.sendtoaddress(node.getnewaddress(), 1)
        self.wait_for_instantlock(cycle_txid)
        self.cycle_islock = self.islock_msg(cycle_txid)
        assert_equal(self.cycle_islock.cycleHash, int(node.getblockhash(self.cycle_of(node.getblockcount())), 16))
        self.generate(node, 1)

        # Two spends of one coin: the honest chain mines the locked one, and
        # node A first mines the other itself.
        coin = next(c for c in node.listunspent() if c["amount"] > 2)
        spends = []
        for _ in range(2):
            raw = node.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],
                                            {node.getnewaddress(): Decimal(coin["amount"]) - Decimal("0.001")})
            spends.append(node.signrawtransactionwithwallet(raw)["hex"])
        assert spends[0] != spends[1]
        self.conflicting_spend = spends[0]
        self.conflict_height = node.getblockcount() + 1
        self.locked_txid = node.sendrawtransaction(spends[1])
        self.wait_for_instantlock(self.locked_txid)
        self.locked_islock = self.islock_msg(self.locked_txid)
        self.generate(node, 1)
        assert self.locked_txid in node.getblock(node.getblockhash(self.conflict_height))["tx"]

        # The verifier also tries the previous cycle's quorums, so the lock is
        # delivered two cycles after its own.
        self.mine_cycle_quorum()
        self.mine_cycle_quorum()
        self.wait_for_chainlocked_block_all_nodes(node.getbestblockhash())
        self.chainlock, self.chainlock_clsig = self.chainlock_msg()
        assert_equal(self.chainlock["height"], node.getblockcount())

        self.old_ehf_height = node.getblockcount() + 1
        node.sendrawtransaction(self.old_ehf.serialize().hex(), maxfeerate=0)
        self.generate(node, 1)
        assert_equal(len(node.getblock(node.getbestblockhash())["tx"]), 2)
        self.move_blocks(self.nodes, 3)
        self.tip = node.getbestblockhash()

    # Importers ---------------------------------------------------------------

    def add_cold_node(self, name, spork_key=False):
        """A node with only the headers, which authorizes all three dumps."""
        self.log.info(f"Start node {name}, which knows only the headers")
        idx = len(self.nodes)
        args = list(self.extra_args[0]) + [self.pin(d) for d in (self.dump_x, self.dump_r, self.dump_a)]
        if spork_key:
            # Its only peer is the test, so it signs the sporks node 0 set itself.
            args.append(f"-sporkkey={SPORK_KEY}")
        initialize_datadir(self.options.tmpdir, idx, self.chain, self.disable_autoconnect)
        self.add_nodes(1, extra_args=[args])
        self.importer_args[idx] = args
        self.start_node(idx)
        importer = self.nodes[idx]
        self.submit_headers(importer)
        if spork_key:
            for spork in ("SPORK_2_INSTANTSEND_ENABLED", "SPORK_19_CHAINLOCKS_ENABLED"):
                importer.sporkupdate(spork, 0)
        force_finish_mnsync(importer)
        return idx

    def submit_headers(self, importer):
        node = self.nodes[0]
        for height in range(1, node.getblockcount() + 1):
            importer.submitheader(node.getblockheader(node.getblockhash(height), False))
        assert_equal(importer.getblockchaininfo()["headers"], node.getblockcount())
        assert_equal(importer.getblockcount(), 0)

    def submit_blocks(self, importer, first, last):
        node = self.nodes[0]
        for height in range(first, last + 1):
            importer.submitblock(node.getblock(node.getblockhash(height), 0))

    def chainstates(self, importer):
        return importer.getchainstates()["chainstates"]

    def assert_loaded(self, importer, dump):
        loaded = importer.loadtxoutset(dump["path"])
        assert_equal(loaded["coins_loaded"], dump["coins_written"])
        assert_equal(loaded["base_height"], dump["base_height"])
        normal, snapshot = self.chainstates(importer)
        assert_equal(normal["blocks"], 0)
        assert_equal(snapshot["blocks"], dump["base_height"])
        assert_equal(snapshot["snapshot_blockhash"], dump["base_hash"])
        assert_equal(snapshot["validated"], False)

    def invalid_tips(self, importer):
        return [tip["hash"] for tip in importer.getchaintips() if tip["status"] == "invalid"]

    def snapshot_dir(self, importer, suffix=""):
        return importer.chain_path / f"chainstate_snapshot{suffix}"

    def assert_no_snapshot(self, importer):
        chainstate, = self.chainstates(importer)
        assert_equal(chainstate.get("snapshot_blockhash"), None)
        assert not self.snapshot_dir(importer).exists()

    def assert_refused(self, importer, path, log_msg):
        with importer.assert_debug_log([log_msg]):
            assert_raises_rpc_error(-32603, "Unable to load UTXO snapshot", importer.loadtxoutset, path)
        assert not self.snapshot_dir(importer).exists()

    def restart(self, idx, extra_args=None):
        self.restart_node(idx, extra_args=self.importer_args[idx] + (extra_args or []))
        force_finish_mnsync(self.nodes[idx])

    def sync_from_blocks(self, idx):
        """Connect a node without a snapshot to node 0 and let it sync."""
        importer = self.nodes[idx]
        self.connect_nodes(idx, 0)
        self.wait_until(lambda: importer.getbestblockhash() == self.tip)
        assert_equal(importer.gettxoutsetinfo()["hash_serialized_3"], self.nodes[0].gettxoutsetinfo()["hash_serialized_3"])
        self.disconnect_nodes(idx, 0)
        self.log.info("Restart, so that LoadBlockIndex and CheckBlockIndex run over the synced chain")
        self.restart(idx)
        assert_equal(importer.getbestblockhash(), self.tip)

    # Scenarios ---------------------------------------------------------------

    def test_mutated_snapshots_and_reindex(self):
        idx = self.add_cold_node("X")
        importer = self.nodes[idx]
        dump = self.dump_x
        contents = Path(dump["path"]).read_bytes()

        self.log.info("A mutated coin, a mutated evo section and either pinned hash are each refused")
        vout_offset = 32 + 8 + 32
        bad_path = self.write_flipped(dump, vout_offset)
        self.assert_refused(importer, bad_path, f"[snapshot] bad snapshot content hash: expected {dump['txoutset_hash']}, got ")
        # The first credit-pool window entry, a fixed-size amount near the end.
        credit_offset = len(contents) - 1 - 5 * dump["evo"]["mnhf_signals"] - 16 * CREDIT_POOL_PERIOD
        assert_equal(contents[credit_offset - 1], CREDIT_POOL_PERIOD)
        bad_path = self.write_flipped(dump, credit_offset)
        self.assert_refused(importer, bad_path, f"[snapshot] bad evo section hash: expected {dump['evo']['hash']}, got ")
        for evo_hash, txoutset_hash, log_msg in [
            (self.dump_r["evo"]["hash"], None, f"[snapshot] bad evo section hash: expected {self.dump_r['evo']['hash']}, got {dump['evo']['hash']}"),
            (None, self.dump_r["txoutset_hash"], f"[snapshot] bad snapshot content hash: expected {self.dump_r['txoutset_hash']}, got {dump['txoutset_hash']}"),
        ]:
            self.restart_node(idx, extra_args=self.args_with_pin(idx, dump, evo_hash=evo_hash, txoutset_hash=txoutset_hash))
            self.assert_refused(importer, dump["path"], log_msg)
        self.restart(idx)
        self.assert_no_snapshot(importer)
        assert_equal(importer.getblockcount(), 0)

        self.log.info("-reindex drops a snapshot whose chain connected blocks above the base")
        self.assert_loaded(importer, dump)
        top = self.dump_a["base_height"]
        self.submit_blocks(importer, dump["base_height"] + 1, top)
        assert_equal(self.chainstates(importer)[-1]["blocks"], top)
        self.restart(idx, extra_args=["-reindex"])
        self.assert_no_snapshot(importer)
        assert_equal(importer.getblockcount(), 0)

        self.log.info("After the reindex the node loads again, and -reindex-chainstate drops that snapshot too")
        # The reindex kept only what the block files hold.
        self.submit_headers(importer)
        self.assert_loaded(importer, dump)
        self.submit_blocks(importer, dump["base_height"] + 1, top)
        assert_equal(self.chainstates(importer)[-1]["blocks"], top)
        self.restart(idx, extra_args=["-reindex-chainstate"])
        self.assert_no_snapshot(importer)

        self.log.info("Plain IBD over the blocks the dropped snapshot connected")
        self.sync_from_blocks(idx)

    def test_rejected_at_completion(self):
        idx = self.add_cold_node("R")
        importer = self.nodes[idx]
        dump = self.dump_r

        self.log.info("An evo section differing in an EHF signal only completion checks, under its own pin")
        contents = Path(dump["path"]).read_bytes()
        signals = dump["evo"]["mnhf_signals"]
        tail = len(contents) - 5 * signals - 1
        assert_equal(contents[tail], signals)
        entries = sorted([contents[tail + 1 + 5 * i:tail + 6 + 5 * i] for i in range(signals)] +
                         [bytes([OLD_QUORUM_EHF_BIT - 1]) + struct.pack("<i", dump["base_height"] - 1)])
        altered = contents[:tail] + bytes([signals + 1]) + b"".join(entries)
        evo_hash = hashlib.sha256(altered[altered.find(EVO_MARKER) + len(EVO_MARKER):]).digest()[::-1].hex()
        altered_path = dump["path"] + ".altered"
        Path(altered_path).write_bytes(altered)
        self.importer_args[idx] = self.args_with_pin(idx, dump, evo_hash=evo_hash) + ["-persistmempool=0"]
        self.restart(idx)
        altered_dump = dict(dump, path=altered_path)
        self.assert_loaded(importer, altered_dump)

        self.log.info("Completion rejects it and stops the node, blaming no block and no peer")
        base = dump["base_height"]
        self.submit_blocks(importer, base + 1, base + 3)
        self.wait_until(lambda: self.chainstates(importer)[-1]["blocks"] == base + 3)
        node = self.nodes[0]
        # A child of a transaction mined above the base is valid only on the
        # snapshot chain.
        parent = node.gettransaction(self.r_parent_txid)
        assert_equal(parent["blockheight"], base + 1)
        raw = node.createrawtransaction([{"txid": self.r_parent_txid, "vout": self.r_parent_vout}],
                                        {node.getnewaddress(): Decimal("0.999")})
        child = node.signrawtransactionwithwallet(raw)
        assert child["complete"]
        c_txid = importer.sendrawtransaction(child["hex"])
        assert_equal(importer.getrawmempool(), [c_txid])
        blocks = []
        for height in range(1, dump["base_height"] + 1):
            block = from_hex(CBlock(), node.getblock(node.getblockhash(height), 0))
            block.rehash()
            blocks.append(block)
        server = importer.add_p2p_connection(BlockServer(blocks))
        with importer.assert_debug_log(expected_msgs=["[snapshot] the evo section differs from the validated state",
                                                      "[snapshot] deleting snapshot, reverting to validated chain, and stopping node"],
                                       unexpected_msgs=["Misbehaving"], timeout=60):
            server.send_message(msg_headers(server.headers))
            importer.wait_until_stopped(timeout=60, expect_error=True, expected_stderr=(
                "Error: Dash Core failed to validate the -assumeutxo snapshot state. This indicates a hardware problem, "
                "or a bug in the software, or a bad software modification that allowed an invalid snapshot to be loaded. "
                "As a result of this, the node will shut down and stop using any state that was built on the snapshot, "
                f"resetting the chain height from {base + 3} to {base}. On the next restart, the node will resume syncing "
                f"from {base} without using any snapshot data. Please report this incident to "
                "https://github.com/dashpay/dash/issues, including how you obtained the snapshot. The invalid snapshot "
                "chainstate will be left on disk in case it is helpful in diagnosing the issue that caused this error."))
        assert self.snapshot_dir(importer, "_INVALID").exists()

        self.log.info("After the restart the node is back on its validated chain and marks no block invalid")
        self.restart(idx)
        self.assert_no_snapshot(importer)
        assert_equal(importer.getblockcount(), base + 3)
        assert_equal(self.invalid_tips(importer), [])

        self.log.info("Another snapshot is refused while the rejected one is kept")
        self.assert_refused(importer, self.dump_a["path"],
                            f"[snapshot] refusing to activate snapshot {self.dump_a['base_hash']}: an earlier rejected snapshot chainstate is still at")
        assert self.snapshot_dir(importer, "_INVALID").exists()

        self.log.info("Plain IBD after the rejection")
        self.sync_from_blocks(idx)

    def test_dual_mode(self):
        idx = self.add_cold_node("A", spork_key=True)
        importer = self.nodes[idx]
        node = self.nodes[0]
        dump = self.dump_a
        base = dump["base_height"]
        self.assert_loaded(importer, dump)

        self.log.info("A restart before any block keeps both chainstates")
        self.restart(idx)
        assert_equal([c["blocks"] for c in self.chainstates(importer)], [0, base])

        self.log.info("Below the base the snapshot chain answers deployment state from the carried signals")
        for height in (base - 1, base):
            h = node.getblockhash(height)
            assert_equal(importer.getdeploymentinfo(h), node.getdeploymentinfo(h))

        self.log.info("A transaction spending snapshot coins enters the mempool, and a carried quorum's lock for it verifies")
        importer.sendrawtransaction(node.getrawtransaction(self.is_txid))
        relay = importer.add_p2p_connection(LockRelay())
        relay.announce(self.islock_msg(self.is_txid), 31)
        self.wait_until(lambda: importer.getrawtransaction(self.is_txid, True)["instantlock_internal"])

        self.log.info("The snapshot chain connects InstantSend and withdrawal traffic above the base")
        self.submit_blocks(importer, base + 1, self.conflict_height - 1)
        is_block = node.getrawtransaction(self.is_txid, True)["blockhash"]
        assert importer.getrawtransaction(self.is_txid, True, is_block)["instantlock_internal"]
        before_conflict = node.getblockhash(self.conflict_height - 1)
        assert_equal(importer.getbestblockhash(), before_conflict)
        assert_equal(importer.getdeploymentinfo(), node.getdeploymentinfo(before_conflict))
        index = str(WITHDRAWAL_INDEX)
        # Mined below a block whose coinbase carries a ChainLock.
        assert_equal(importer.getassetunlockstatuses([index]), [{"index": WITHDRAWAL_INDEX, "status": "chainlocked"}])
        # A coin from the snapshot spends in the mempool. It is mature at the
        # snapshot chain's tip even if it is a coinbase.
        coin = next(c for c in node.listunspent() if c["confirmations"] > node.getblockcount() - base + 110)
        raw = node.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],
                                        {node.getnewaddress(): Decimal(coin["amount"]) - Decimal("0.001")})
        signed = node.signrawtransactionwithwallet(raw)["hex"]
        assert_equal(importer.testmempoolaccept([signed])[0].get("reject-reason"), None)

        self.log.info("The carried platform quorum verifies a withdrawal signature")
        assert node.quorum("info", LLMQ_TEST_PLATFORM, self.platform_quorum)["height"] < base
        assert importer.quorum("verify", LLMQ_TEST_PLATFORM, self.platform_recsig["id"], self.platform_recsig["msgHash"],
                               self.platform_recsig["sig"], self.platform_quorum)

        self.log.info("An InstantSend lock disconnects a block with a conflicting spend")
        conflicting_txid = importer.sendrawtransaction(self.conflicting_spend)
        # With ChainLocks on, a transaction without a lock is mined once it is 10 minutes old.
        self.bump_mocktime(11 * 60, nodes=[importer])
        conflicting_block = self.generate(importer, 1, sync_fun=self.no_op)[0]
        assert conflicting_txid in importer.getblock(conflicting_block)["tx"]
        with importer.assert_debug_log([f"invalidating block {conflicting_block}",
                                        f"resetting block {conflicting_block}"],
                                       unexpected_msgs=["retrying later", "Misbehaving"]):
            relay.announce(self.locked_islock, 31)
            self.wait_until(lambda: importer.getbestblockhash() == before_conflict)
        # Without the locked transaction, the invalidation is temporary.
        assert_equal(self.invalid_tips(importer), [])
        # The locked spend replaces the one the disconnected block returned to the mempool.
        assert conflicting_txid in importer.getrawmempool()
        importer.sendrawtransaction(node.getrawtransaction(self.locked_txid))
        assert_equal(importer.getrawmempool(), [self.locked_txid])
        self.wait_until(lambda: importer.getrawtransaction(self.locked_txid, True)["instantlock_internal"])

        self.log.info("The snapshot chain follows the locked spend")
        stall_height = self.old_ehf_height
        self.submit_blocks(importer, self.conflict_height, stall_height - 1)
        assert_equal(importer.getbestblockhash(), node.getblockhash(stall_height - 1))
        # Tried again with the lock known, the block with the other spend is invalid.
        assert_equal(self.invalid_tips(importer), [conflicting_block])

        self.log.info("The background chainstate connects blocks before v19, then a basic-scheme ChainLock is accepted")
        self.submit_blocks(importer, 1, V19_HEIGHT - 2)
        assert_equal([c["blocks"] for c in self.chainstates(importer)], [V19_HEIGHT - 2, stall_height - 1])
        assert not importer.getdeploymentinfo(importer.getblockhash(V19_HEIGHT - 2))["deployments"]["v19"]["active"]
        # The invalid block has more work than the background tip, which is not
        # a reason to warn about a fork.
        assert "We do not appear to fully agree" not in importer.getblockchaininfo()["warnings"]
        with importer.assert_debug_log(expected_msgs=[], unexpected_msgs=["Misbehaving"]):
            relay.announce(self.chainlock_clsig, 29)
            self.wait_until(lambda: importer.getbestchainlock()["blockhash"] == self.chainlock["blockhash"])
        assert relay.is_connected

        self.log.info("An EHF signal of a quorum the snapshot does not carry stalls the snapshot chain")
        stall_hash = node.getblockhash(stall_height)
        with importer.assert_debug_log([f"stalled at block {stall_hash} (height {stall_height})"]):
            assert_raises_rpc_error(-25, "evo-local-unavailable", importer.submitblock, node.getblock(stall_hash, 0))
        assert_equal(importer.getbestblockhash(), node.getblockhash(stall_height - 1))
        assert_equal(self.invalid_tips(importer), [conflicting_block])
        assert_equal(importer.getblockheader(stall_hash)["chainlock"], False)
        importer.disconnect_p2ps()

        self.log.info("A restart while stalled keeps the stall")
        with importer.assert_debug_log([f"stalled at block {stall_hash} (height {stall_height})"]):
            self.restart(idx)
        assert_equal(importer.getbestblockhash(), node.getblockhash(stall_height - 1))

        self.log.info("Background validation completes in the same run while the snapshot chain stays stalled")
        self.connect_nodes(idx, 0)
        self.wait_until(lambda: len(self.chainstates(importer)) == 1)
        assert_equal(importer.getbestblockhash(), node.getblockhash(stall_height - 1))
        self.disconnect_nodes(idx, 0)

        self.log.info("The indexes follow the snapshot chain, and completion announced no historical block to InstantSend")
        self.wait_until(lambda: importer.getindexinfo("txindex")["txindex"]["best_block_height"] == stall_height - 1)
        relay = importer.add_p2p_connection(LockRelay())
        relay_id = importer.getpeerinfo()[-1]["id"]
        cycle_islock_hash = f"{uint256_from_str(hash256(self.cycle_islock.serialize())):064x}"
        with importer.assert_debug_log(expected_msgs=[f"islock={cycle_islock_hash}: processing islock, peer={relay_id}"],
                                       unexpected_msgs=["Misbehaving", "verification failed"], timeout=10):
            relay.announce(self.cycle_islock, 31)
            relay.sync_with_ping()
            importer.syncwithvalidationinterfacequeue()
        assert relay.is_connected
        importer.disconnect_p2ps()

        self.log.info("A validated, unpromoted snapshot keeps a masternode key from starting")
        self.stop_node(idx)
        importer.assert_start_raises_init_error(
            extra_args=self.importer_args[idx] + [f"-masternodeblsprivkey={self.mninfo[0].keyOperator}"],
            expected_msg=f"Error: {MASTERNODE_REFUSAL}", match=ErrorMatch.PARTIAL_REGEX)

        self.log.info("The restart promotes the snapshot and the stalled block connects")
        self.restart(idx)
        self.assert_no_snapshot(importer)
        self.sync_from_blocks(idx)
        assert_equal(importer.getdeploymentinfo(), node.getdeploymentinfo())


if __name__ == "__main__":
    AssumeutxoDashTest().main()
