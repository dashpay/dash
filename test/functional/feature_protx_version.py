#!/usr/bin/env python3
# Copyright (c) 2015-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_protx_version.py

Checks ProTx versioning across the v19 (basic BLS) and v24 (extended
addresses) forks.

'''
from io import BytesIO

from test_framework.p2p import P2PInterface
from test_framework.messages import CBlock, CBlockHeader, CCbTx, CMerkleBlock, from_hex, hash256, msg_getmnlistd, \
    QuorumId, ser_uint256
from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import (
    assert_equal,
    softfork_active,
)


class TestP2PConn(P2PInterface):
    def __init__(self):
        super().__init__()
        self.last_mnlistdiff = None

    def on_mnlistdiff(self, message):
        self.last_mnlistdiff = message

    def wait_for_mnlistdiff(self, timeout=30):
        def received_mnlistdiff():
            return self.last_mnlistdiff is not None
        return self.wait_until(received_mnlistdiff, timeout=timeout)

    def getmnlistdiff(self, base_block_hash, block_hash):
        msg = msg_getmnlistd(base_block_hash, block_hash)
        self.last_mnlistdiff = None
        self.send_message(msg)
        self.wait_for_mnlistdiff()
        return self.last_mnlistdiff


class ProTxVersionTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.extra_args = [[
            '-deprecatedrpc=legacy_mn',
            '-testactivationheight=v19@200',
            # Wide enough a window that v24 does not activate on its own during the body of this
            # test; it is activated explicitly at the end.
            f'-vbparams=v24:{self.mocktime}:999999999999:350:10:8:6:5:0',
        ]] * 2
        self.set_dash_test_params(2, 1, evo_count=2, extra_args=self.extra_args)

    def get_peer_ids(self, node_idx):
        return {peer['id'] for peer in self.nodes[node_idx].getpeerinfo()}

    def wait_for_peers_disconnected(self, node_idx, peer_ids):
        """Wait until none of `peer_ids` is connected to node `node_idx` anymore.

        Don't wait for getconnectioncount() to hit zero instead: intra-quorum connections are
        re-established concurrently, as the masternode remains a member of previously formed
        quorums, so zero might never be observable.
        """
        assert peer_ids, "no peers to wait for, the disconnect assertion would pass trivially"
        self.wait_until(lambda: peer_ids.isdisjoint(self.get_peer_ids(node_idx)))

    def run_test(self):
        # Connect all nodes to node1 so that we always have the whole network connected
        # Otherwise only masternode connections will be established between nodes, which won't propagate TXs/blocks
        # Usually node0 is the one that does this, but in this test we isolate it multiple times

        self.test_node = self.nodes[0].add_p2p_connection(TestP2PConn())
        null_hash = format(0, "064x")

        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        expected_updated = [mn.proTxHash for mn in self.mninfo]
        b_0 = self.nodes[0].getbestblockhash()
        self.test_getmnlistdiff(null_hash, b_0, {}, [], expected_updated)

        extra_legacy_mn: MasternodeInfo = self.dynamically_add_masternode()
        assert extra_legacy_mn is not None

        # A second legacy-scheme masternode kept alive (never revoked) until v24 activates, so that
        # the update_registrar migration path with a rotated key (legacy state -> basic scheme) can
        # be exercised below.
        migrate_legacy_mn: MasternodeInfo = self.dynamically_add_masternode()
        assert migrate_legacy_mn is not None

        # A third legacy-scheme masternode, also never revoked, used to exercise the in-place
        # same-key migration path (update_service, no key rotation) once v24 activates.
        surviving_legacy_mn: MasternodeInfo = self.dynamically_add_masternode()
        assert surviving_legacy_mn is not None

        mn_list_before = self.nodes[0].masternodelist()
        pubkeyoperator_list_before = set([mn_list_before[e]["pubkeyoperator"] for e in mn_list_before])

        self.mine_quorum(llmq_type_name='llmq_test', llmq_type=100)

        self.activate_by_name('v19', expected_activation_height=200)
        self.log.info("Activated v19 at height:" + str(self.nodes[0].getblockcount()))

        mn_list_after = self.nodes[0].masternodelist()
        pubkeyoperator_list_after = set([mn_list_after[e]["pubkeyoperator"] for e in mn_list_after])

        self.log.info("pubkeyoperator should still be shown using legacy scheme")
        assert_equal(pubkeyoperator_list_before, pubkeyoperator_list_after)

        evo_info_0: MasternodeInfo = self.dynamically_add_masternode(evo=True, rnd=7)
        assert evo_info_0 is not None

        self.log.info("Checking that protxs with duplicate EvoNodes fields are rejected")
        evo_info_1: MasternodeInfo = self.dynamically_add_masternode(evo=True, rnd=7, should_be_rejected=True)
        assert evo_info_1 is None
        self.dynamically_evo_update_service(evo_info_0, 8)
        evo_info_2: MasternodeInfo = self.dynamically_add_masternode(evo=True, rnd=8, should_be_rejected=True)
        assert evo_info_2 is None
        evo_info_3: MasternodeInfo = self.dynamically_add_masternode(evo=True, rnd=9)
        assert evo_info_3 is not None
        self.dynamically_evo_update_service(evo_info_0, 9, should_be_rejected=True)

        # Test revoking a post-V19 masternode (basic BLS scheme)
        self.log.info(f"Trying to revoke post-V19 proTx:{evo_info_3.proTxHash}")
        self.test_revoke_protx(evo_info_3.nodeIdx, self.mninfo[-1])

        # Test updating and revoking a pre-V19 masternode (legacy BLS scheme) after V19 activation
        self.log.info(f"Trying to update pre-V19 proTx:{extra_legacy_mn.proTxHash}")
        self.test_update_service_protx(extra_legacy_mn)
        self.log.info(f"Trying to revoke pre-V19 (legacy) proTx:{extra_legacy_mn.proTxHash}")
        self.test_revoke_protx(extra_legacy_mn.nodeIdx, extra_legacy_mn)

        # Avoid including these masternodes in next dkg to improve test stability
        self.move_blocks(self.nodes, 24)

        self.mine_quorum(llmq_type_name='llmq_test', llmq_type=100)

        self.log.info("Checking that adding more regular MNs after v19 doesn't break DKGs and IS/CLs")

        for i in range(3):
            new_mn: MasternodeInfo = self.dynamically_add_masternode(evo=False, rnd=(10 + i))
            assert new_mn is not None
            if i == 0:
                # Kept at its basic (v2) state and revoked after v24 to check the version is preserved
                basic_mn = new_mn
            if i == 1:
                # Kept at its basic (v2) state and update_service'd after v24 to check the payout is preserved
                payout_mn = new_mn

        # mine more quorums and make sure everything still works between v19 and v24
        prev_quorum = None
        for _ in range(2):
            quorum = self.mine_quorum()
            assert prev_quorum != quorum
            self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())

        self.test_protx_v24_versioning(new_mn, migrate_legacy_mn, surviving_legacy_mn, basic_mn, payout_mn, evo_info_0)


    def test_protx_v24_versioning(self, mn: MasternodeInfo, legacy_mn: MasternodeInfo,
                                  surviving_legacy_mn: MasternodeInfo, basic_mn: MasternodeInfo,
                                  payout_mn: MasternodeInfo, basic_evo: MasternodeInfo):
        assert not softfork_active(self.nodes[0], 'v24')
        self.activate_by_name('v24', slow_mode=False)
        self.log.info("Activated v24 at height:" + str(self.nodes[0].getblockcount()))

        node = self.nodes[0]

        self.log.info("update_service bumping a basic (v2) masternode to v3 preserves its owner payout")
        state = node.protx('info', payout_mn.proTxHash)['state']
        assert_equal(state['version'], 2)
        payout_before = state['payoutAddress']
        node.sendtoaddress(payout_mn.fundsAddr, 1)
        upserv_hash = payout_mn.update_service(node, submit=True, addrs_core_p2p=[f'127.0.0.1:{payout_mn.nodePort}'])
        self.bury_tx(node, upserv_hash)
        state = node.protx('info', payout_mn.proTxHash)['state']
        assert_equal(state['version'], 3)
        assert_equal([p['address'] for p in state['payouts']], [payout_before])
        node.sendtoaddress(mn.fundsAddr, 1)

        self.log.info("update_registrar on a basic (v2) EvoNode keeps it at v2, since only an ExtAddr service "
                      "update may raise an EvoNode that has addresses")
        assert_equal(node.protx('info', basic_evo.proTxHash)['state']['version'], 2)
        node.sendtoaddress(basic_evo.fundsAddr, 1)
        payout_address = node.getnewaddress()
        upreg_hash = basic_evo.update_registrar(node, submit=True, rewards_address=payout_address,
                                                fundsAddr=basic_evo.fundsAddr)
        tip = self.bury_tx(node, upreg_hash)
        assert_equal(node.getrawtransaction(upreg_hash, 1, tip)['proUpRegTx']['version'], 2)
        state = node.protx('info', basic_evo.proTxHash)['state']
        assert_equal(state['version'], 2)
        assert_equal(state['payoutAddress'], payout_address)
        basic_evo.update_registrar(node, submit=True, payouts=[{'address': payout_address, 'reward': 5000},
                                                                {'address': node.getnewaddress(), 'reward': 5000}],
                                   fundsAddr=basic_evo.fundsAddr, expected_assert_code=-8,
                                   expected_assert_msg="multiple payouts require the EvoNode to be upgraded first")

        self.log.info("A basic-scheme masternode reports version 2 before any post-v24 update")
        assert_equal(node.protx('info', mn.proTxHash)['state']['version'], 2)

        self.log.info("A v3 ProUpRegTx reusing the basic operator key is accepted, but leaves the "
                      "stored masternode version at 3 even no operator key change took place")
        protx_result = mn.update_registrar(node, submit=True, fundsAddr=mn.fundsAddr)
        tip = self.bury_tx(node, protx_result)
        assert_equal(node.getrawtransaction(protx_result, 1, tip)['proUpRegTx']['version'], 3)
        assert_equal(node.protx('info', mn.proTxHash)['state']['version'], 3)

        self.log.info("Migration v1 [legacy] protx masternode to v3 by update-registar with a rotated key")
        assert_equal(node.protx('info', legacy_mn.proTxHash)['state']['version'], 1)
        node.sendtoaddress(legacy_mn.fundsAddr, 1)
        # Switch to a fresh basic-scheme operator key and the non-legacy update_registrar RPC
        legacy_mn.legacy = False
        new_operator = node.bls('generate') # basic (non-legacy) scheme
        legacy_mn.pubKeyOperator = new_operator['public']
        legacy_mn.keyOperator = new_operator['secret']
        migrate_result = legacy_mn.update_registrar(node, submit=True, fundsAddr=legacy_mn.fundsAddr)
        assert legacy_mn.nodeIdx is not None
        old_peer_ids = self.get_peer_ids(legacy_mn.nodeIdx)
        tip = self.bury_tx(node, migrate_result, sync_fun=self.no_op)
        assert_equal(node.getrawtransaction(migrate_result, 1, tip)['proUpRegTx']['version'], 3)
        assert_equal(node.protx('info', legacy_mn.proTxHash)['state']['version'], 3)
        # Changing the operator key makes every peer drop its existing connections to this
        # masternode. Wait for that to happen and then reconnect its node back to let sync_all
        # finish correctly.
        self.wait_for_peers_disconnected(legacy_mn.nodeIdx, old_peer_ids)
        self.connect_nodes(legacy_mn.nodeIdx, 0)
        self.sync_all()

        self.test_revoke_protx(mn.nodeIdx, mn)

        self.log.info("Masternode list reloads from disk identically after the v3 updates")
        list_before = self.nodes[1].masternodelist()
        self.restart_node(1, extra_args=self.extra_args[1])
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        assert_equal(self.nodes[1].masternodelist(), list_before)

        self.log.info("Revoking a still-v2 (BasicBLS) masternode after v24 preserves its state version "
                      "instead of silently downgrading it to LegacyBLS via the operator-field reset")
        assert_equal(node.protx('info', basic_mn.proTxHash)['state']['version'], 2)
        self.test_revoke_protx(basic_mn.nodeIdx, basic_mn)
        # The v3 ProUpRevTx is applied against a v2 state, so max(old, tx) keeps the version at 3, not 1
        assert_equal(node.protx('info', basic_mn.proTxHash)['state']['version'], 3)

        # A legacy masternode can instead migrate to the basic scheme in place, keeping the same
        # operator key: a v3 service update re-encodes the stored key (SetStateVersion) and re-keys
        # the unique-property map. No key rotation is forced, so the masternode is not PoSe-banned.
        # Run last: the key re-encoding is a key change for masternode authentication (CMNAuth) and
        # briefly churns the migrated node's masternode connections, so keep it after the checks above
        # to avoid destabilising them.
        self.log.info("post-v24: update_service migrates a legacy masternode to the basic scheme in place")
        assert_equal(node.protx('info', surviving_legacy_mn.proTxHash)['state']['version'], 1)
        key_before = node.protx('info', surviving_legacy_mn.proTxHash)['state']['pubKeyOperator']
        node.sendtoaddress(surviving_legacy_mn.fundsAddr, 1)
        upserv_hash = surviving_legacy_mn.update_service(node, submit=True,
                                                         addrs_core_p2p=[f'127.0.0.1:{surviving_legacy_mn.nodePort}'])
        # Same CMNAuth disconnect as a key rotation: SetStateVersion re-encodes pubKeyOperator, so
        # skip sync_all until the old peers are gone and this node is reconnected.
        assert surviving_legacy_mn.nodeIdx is not None
        old_peer_ids = self.get_peer_ids(surviving_legacy_mn.nodeIdx)
        tip = self.bury_tx(node, upserv_hash, sync_fun=self.no_op)
        assert_equal(node.getrawtransaction(upserv_hash, 1, tip)['proUpServTx']['version'], 3)
        state = node.protx('info', surviving_legacy_mn.proTxHash)['state']
        assert_equal(state['version'], 3)  # migrated in place
        # Same operator key, re-encoded to the basic scheme (different hex, but not a rotation: the
        # masternode is not PoSe-banned).
        assert state['pubKeyOperator'] != key_before
        assert_equal(state['PoSeBanHeight'], -1)
        self.wait_for_peers_disconnected(surviving_legacy_mn.nodeIdx, old_peer_ids)
        self.connect_nodes(surviving_legacy_mn.nodeIdx, 0)
        self.sync_all()
        list_before = self.nodes[1].masternodelist()
        self.restart_node(1, extra_args=self.extra_args[1])
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        assert_equal(self.nodes[1].masternodelist(), list_before)

    def test_revoke_protx(self, node_idx, revoke_mn: MasternodeInfo):
        funds_address = self.nodes[0].getnewaddress()
        self.nodes[0].sendtoaddress(funds_address, 1)

        protx_result = revoke_mn.revoke(self.nodes[0], submit=True, reason=1, fundsAddr=funds_address)
        old_peer_ids = self.get_peer_ids(node_idx)
        self.bury_tx(self.nodes[0], protx_result, sync_fun=self.no_op)
        # Revoking a MN makes every peer drop its existing connections to it. Wait for that to
        # happen and then reconnect the corresponding node back to let sync_blocks finish
        # correctly.
        self.wait_for_peers_disconnected(node_idx, old_peer_ids)
        self.connect_nodes(node_idx, 0)
        self.sync_all()
        self.log.info(f"Successfully revoked={revoke_mn.proTxHash}")
        for mn in self.mninfo: # type: MasternodeInfo
            if mn.proTxHash == revoke_mn.proTxHash:
                self.mninfo.remove(mn)
                return

    def test_update_service_protx(self, mn: MasternodeInfo):
        self.nodes[0].sendtoaddress(mn.fundsAddr, 1)

        protx_result = mn.update_service(self.nodes[0], submit=True, addrs_core_p2p=[f'127.0.0.2:{mn.nodePort}'])
        self.bury_tx(self.nodes[0], protx_result)

        for node in self.nodes:
            protx_info = node.protx('info', mn.proTxHash)
            mn_list = node.masternode('list')
            assert_equal(protx_info['state']['addresses']['core_p2p'][0], '127.0.0.2:%d' % mn.nodePort)
            assert_equal(mn_list['%s-%d' % (mn.collateral_txid, mn.collateral_vout)]['addresses']['core_p2p'][0], '127.0.0.2:%d' % mn.nodePort)
        self.log.info(f"Successfully updated={mn.proTxHash}")

    def test_getmnlistdiff(self, base_block_hash, block_hash, base_mn_list, expected_deleted, expected_updated):
        d = self.test_getmnlistdiff_base(base_block_hash, block_hash)

        # Assert that the deletedMNs and mnList fields are what we expected
        assert_equal(set(d.deletedMNs), set([int(e, 16) for e in expected_deleted]))
        assert_equal(set([e.proRegTxHash for e in d.mnList]), set(int(e, 16) for e in expected_updated))

        # Build a new list based on the old list and the info from the diff
        new_mn_list = base_mn_list.copy()
        for e in d.deletedMNs:
            new_mn_list.pop(format(e, '064x'))
        for e in d.mnList:
            new_mn_list[format(e.proRegTxHash, '064x')] = e

        cbtx = CCbTx()
        cbtx.deserialize(BytesIO(d.cbTx.vExtraPayload))

        # Verify that the merkle root matches what we locally calculate
        hashes = []
        for mn in sorted(new_mn_list.values(), key=lambda mn: ser_uint256(mn.proRegTxHash)):
            hashes.append(hash256(mn.serialize(with_version = False)))
        merkle_root = CBlock.get_merkle_root(hashes)
        assert_equal(merkle_root, cbtx.merkleRootMNList)

        return new_mn_list

    def test_getmnlistdiff_base(self, base_block_hash, block_hash):
        hexstr = self.nodes[0].getblockheader(block_hash, False)
        header = from_hex(CBlockHeader(), hexstr)

        d = self.test_node.getmnlistdiff(int(base_block_hash, 16), int(block_hash, 16))
        assert_equal(d.baseBlockHash, int(base_block_hash, 16))
        assert_equal(d.blockHash, int(block_hash, 16))

        # Check that the merkle proof is valid
        proof = CMerkleBlock(header, d.merkleProof)
        proof = proof.serialize().hex()
        assert_equal(self.nodes[0].verifytxoutproof(proof), [d.cbTx.hash])

        # Check if P2P messages match with RPCs
        d2 = self.nodes[0].protx("diff", base_block_hash, block_hash)
        assert_equal(d2["baseBlockHash"], base_block_hash)
        assert_equal(d2["blockHash"], block_hash)
        assert_equal(d2["cbTxMerkleTree"], d.merkleProof.serialize().hex())
        assert_equal(d2["cbTx"], d.cbTx.serialize().hex())
        assert_equal(set([int(e, 16) for e in d2["deletedMNs"]]), set(d.deletedMNs))
        assert_equal(set([int(e["proRegTxHash"], 16) for e in d2["mnList"]]), set([e.proRegTxHash for e in d.mnList]))
        assert_equal(set([QuorumId(e["llmqType"], int(e["quorumHash"], 16)) for e in d2["deletedQuorums"]]), set(d.deletedQuorums))
        assert_equal(set([QuorumId(e["llmqType"], int(e["quorumHash"], 16)) for e in d2["newQuorums"]]), set([QuorumId(e.llmqType, e.quorumHash) for e in d.newQuorums]))

        return d


if __name__ == '__main__':
    ProTxVersionTest().main()
