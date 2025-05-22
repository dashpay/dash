#!/usr/bin/env python3
# Copyright (c) 2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test network information fields across RPCs."""

from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error
)
from test_framework.script import (
    hash160
)
from test_framework.test_framework import (
    BitcoinTestFramework,
    p2p_port,
    EVONODE_COLLATERAL,
    MASTERNODE_COLLATERAL
)
from test_framework.test_node import TestNode

from _decimal import Decimal
from random import randint

class Node:
    address_collateral: str = ""
    address_funds: str = ""
    address_owner: str = ""
    address_reward: str = ""
    address_voting: str = ""
    is_evo: bool = False
    collateral_txid: str = ""
    collateral_vout: int = 0
    node: TestNode
    node_idx: int = 0
    operator_pk: str = ""
    operator_sk: str = ""
    platform_nodeid: str = ""
    port_p2p: int = 0
    provider_txid: str = ""

    def __init__(self, node: TestNode, is_evo: bool):
        self.is_evo = is_evo
        self.node = node
        self.node_idx = node.index
        self.port_p2p = p2p_port(node.index)
        self.generate_addresses()

    def bury_tx(self, test: BitcoinTestFramework, txid: str, depth: int):
        chain_tip = test.generate(self.node, depth)[0]
        assert_equal(self.node.getrawtransaction(txid, 1, chain_tip)['confirmations'], depth)

    def generate_addresses(self, force_all: bool = False):
        if not self.address_collateral or force_all:
            self.address_collateral = self.node.getnewaddress()
        if not self.address_funds or force_all:
            self.address_funds = self.node.getnewaddress()
        if not self.address_owner or force_all:
            self.address_owner = self.node.getnewaddress()
        if not self.address_reward or force_all:
            self.address_reward = self.node.getnewaddress()
        if not self.address_voting or force_all:
            self.address_voting = self.node.getnewaddress()
        if not self.operator_pk or not self.operator_sk or force_all:
            bls_ret = self.node.bls('generate')
            self.operator_pk = bls_ret['public']
            self.operator_sk = bls_ret['secret']

    def generate_collateral(self, test: BitcoinTestFramework):
        while self.node.getbalance() < self.get_collateral_value():
            test.bump_mocktime(1)
            test.generate(self.node, 10, sync_fun=test.no_op)

        self.collateral_txid = self.node.sendmany("", {self.address_collateral: self.get_collateral_value(), self.address_funds: 1})
        self.bury_tx(test, self.collateral_txid, 1)
        self.collateral_vout = self.get_vout_by_value(self.collateral_txid, Decimal(self.get_collateral_value()))
        assert self.collateral_vout != -1

    def get_collateral_value(self) -> int:
        return EVONODE_COLLATERAL if self.is_evo else MASTERNODE_COLLATERAL

    def get_vout_by_value(self, txid: str, value: Decimal) -> int:
        for txout in self.node.getrawtransaction(txid, 1)['vout']:
            if txout['value'] == value:
                return txout['n']
        return -1

    def is_mn_visible(self, _protx_hash = None) -> bool:
        protx_hash = _protx_hash or self.provider_txid
        mn_list = self.node.masternodelist()
        mn_visible = False
        for mn_entry in mn_list:
            dmn = mn_list.get(mn_entry)
            if dmn['proTxHash'] == protx_hash:
                assert_equal(dmn['type'], "Evo" if self.is_evo else "Regular")
                mn_visible = True
        return mn_visible

    def register_mn(self, test: BitcoinTestFramework, submit: bool, addrs_core_p2p, addrs_platform_p2p = None, addrs_platform_http = None, code = None, msg = None) -> str:
        assert((code and msg) or (not code and not msg))
        protx_output: str = ""
        if self.is_evo:
            assert(addrs_platform_http and addrs_platform_p2p)
            self.platform_nodeid = hash160(b'%d' % randint(1, 65535)).hex()
            if code and msg:
                assert_raises_rpc_error(
                    code, msg, self.node.protx, "register_evo", self.collateral_txid, self.collateral_vout, addrs_core_p2p, self.address_owner, self.operator_pk,
                    self.address_voting, 0, self.address_reward, self.platform_nodeid, addrs_platform_p2p, addrs_platform_http, self.address_funds, submit)
                return ""
            else:
                protx_output = self.node.protx(
                    "register_evo", self.collateral_txid, self.collateral_vout, addrs_core_p2p, self.address_owner, self.operator_pk,
                    self.address_voting, 0, self.address_reward, self.platform_nodeid, addrs_platform_p2p, addrs_platform_http, self.address_funds, submit)
        else:
            if code and msg:
                assert_raises_rpc_error(
                    code, msg, self.node.protx, "register", self.collateral_txid, self.collateral_vout, addrs_core_p2p, self.address_owner, self.operator_pk,
                    self.address_voting, 0, self.address_reward, self.address_funds, submit)
                return ""
            else:
                protx_output = self.node.protx(
                    "register", self.collateral_txid, self.collateral_vout, addrs_core_p2p, self.address_owner, self.operator_pk,
                    self.address_voting, 0, self.address_reward, self.address_funds, submit)
        if not submit:
            return ""
        self.provider_txid = protx_output
        self.bury_tx(test, self.provider_txid, 1)
        assert_equal(self.is_mn_visible(), True)
        test.log.debug(f"Registered {'Evo' if self.is_evo else 'regular'} masternode with collateral_txid={self.collateral_txid}, "
                        f"collateral_vout={self.collateral_vout}, provider_txid={self.provider_txid}")
        test.restart_node(self.node_idx, extra_args=self.node.extra_args + [f'-masternodeblsprivkey={self.operator_sk}'])
        return self.provider_txid

    def update_mn(self, test: BitcoinTestFramework, addrs_core_p2p, addrs_platform_p2p = None, addrs_platform_http = None) -> str:
        update_txid: str = ""
        if self.is_evo:
            assert(addrs_platform_http and addrs_platform_p2p)
            update_txid = self.node.protx('update_service_evo', self.provider_txid, addrs_core_p2p, self.operator_sk, self.platform_nodeid,
                                          addrs_platform_p2p, addrs_platform_http, "", self.address_funds)
        else:
            update_txid = self.node.protx('update_service', self.provider_txid, addrs_core_p2p, self.operator_sk, "",
                                          self.address_funds)
        self.bury_tx(test, update_txid, 1)
        assert_equal(self.is_mn_visible(), True)
        test.log.debug(f"Updated {'Evo' if self.is_evo else 'regular'} masternode with collateral_txid={self.collateral_txid}, "
                       f"collateral_vout={self.collateral_vout}, provider_txid={self.provider_txid}")
        return update_txid

    def destroy_mn(self, test: BitcoinTestFramework):
        # Get UTXO from address used to pay fees
        address_funds_unspent = self.node.listunspent(0, 99999, [self.address_funds])[0]
        address_funds_value = address_funds_unspent['amount']

        # Reserve new address for collateral and fee spending
        new_address_collateral = self.node.getnewaddress()
        new_address_funds = self.node.getnewaddress()

        # Create transaction to spend old collateral and fee change
        raw_tx = self.node.createrawtransaction([
                { 'txid': self.collateral_txid, 'vout': self.collateral_vout },
                { 'txid': address_funds_unspent['txid'], 'vout': address_funds_unspent['vout'] }
            ], [
                {new_address_collateral: float(self.get_collateral_value())},
                {new_address_funds: float(address_funds_value - Decimal(0.001))}
            ])
        raw_tx = self.node.signrawtransactionwithwallet(raw_tx)['hex']

        # Send that transaction, resulting txid is new collateral
        new_collateral_txid = self.node.sendrawtransaction(raw_tx)
        self.bury_tx(test, new_collateral_txid, 1)
        new_collateral_vout = self.get_vout_by_value(new_collateral_txid, Decimal(self.get_collateral_value()))
        assert new_collateral_vout != -1

        # Old masternode entry should be dead
        assert_equal(self.is_mn_visible(self.provider_txid), False)
        test.log.debug(f"Destroyed {'Evo' if self.is_evo else 'regular'} masternode with collateral_txid={self.collateral_txid}, "
                       f"collateral_vout={self.collateral_vout}, provider_txid={self.provider_txid}")

        # Generate fresh addresses (and overwrite some of them with addresses used here)
        self.generate_addresses(True)
        self.address_collateral = new_address_collateral
        self.address_funds = new_address_funds
        self.collateral_txid = new_collateral_txid
        self.collateral_vout = new_collateral_vout
        self.provider_txid = ""

        # Restart node sans masternodeblsprivkey
        test.restart_node(self.node_idx, extra_args=self.node.extra_args)

class NetInfoTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            ["-dip3params=2:2"],
            ["-deprecatedrpc=service", "-dip3params=2:2"]
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def check_netinfo_fields(self, val, core_p2p_port: int):
        assert_equal(val[0], f"127.0.0.1:{core_p2p_port}")

    def run_test(self):
        self.node_evo: Node = Node(self.nodes[0], True)
        self.node_simple: TestNode = self.nodes[1]

        self.node_evo.generate_collateral(self)

        self.test_validation()
        self.test_deprecation()

    def test_validation(self):
        self.log.info("Test input validation for masternode address fields")
        # Arrays of addresses are recognized by coreP2PAddrs
        self.node_evo.register_mn(self, False, [f"127.0.0.1:{self.node_evo.port_p2p}", f"127.0.0.2:9998"], "22200", "22201",
                                  -8, f"Error setting coreP2PAddrs[1] to '127.0.0.2:9998' (too many entries)")

        # platformP2PPort and platformHTTPPort doesn't accept non-numeric inputs
        self.node_evo.register_mn(self, False, f"127.0.0.1:{self.node_evo.port_p2p}", "127.0.0.1:22200", "22201",
                                  -8, "platformP2PPort must be a 32bit integer (not '127.0.0.1:22200')")
        self.node_evo.register_mn(self, False, f"127.0.0.1:{self.node_evo.port_p2p}", ["127.0.0.1:22200"], "22201",
                                  -8, "Invalid param for platformP2PPort, must be number")
        self.node_evo.register_mn(self, False, f"127.0.0.1:{self.node_evo.port_p2p}", "22200", "127.0.0.1:22201",
                                  -8, "platformHTTPPort must be a 32bit integer (not '127.0.0.1:22201')")
        self.node_evo.register_mn(self, False, f"127.0.0.1:{self.node_evo.port_p2p}", "22200", ["127.0.0.1:22201"],
                                  -8, "Invalid param for platformHTTPPort, must be number")

        # platformP2PPort and platformHTTPPort must be within acceptable range (i.e. a valid port number)
        self.node_evo.register_mn(self, False, f"127.0.0.1:{self.node_evo.port_p2p}", "0", "22201",
                                  -8, "platformP2PPort must be a valid port [1-65535]")
        self.node_evo.register_mn(self, False, f"127.0.0.1:{self.node_evo.port_p2p}", "65536", "22201",
                                  -8, "platformP2PPort must be a valid port [1-65535]")
        self.node_evo.register_mn(self, False, f"127.0.0.1:{self.node_evo.port_p2p}", "22200", "0",
                                  -8, "platformHTTPPort must be a valid port [1-65535]")
        self.node_evo.register_mn(self, False, f"127.0.0.1:{self.node_evo.port_p2p}", "22200", "65536",
                                  -8, "platformHTTPPort must be a valid port [1-65535]")

    def test_deprecation(self):
        self.log.info("Test output masternode address fields for consistency")

        # netInfo is represented with JSON in CProRegTx, CProUpServTx, CDeterministicMNState and CSimplifiedMNListEntry,
        # so we need to test calls that rely on these underlying implementations. Start by collecting RPC responses.
        self.log.info("Collect JSON RPC responses from node")

        # CProRegTx::ToJson() <- TxToUniv() <- TxToJSON() <- getrawtransaction
        proregtx_hash = self.node_evo.register_mn(self, True, f"127.0.0.1:{self.node_evo.port_p2p}", "22200", "22201")
        proregtx_rpc = self.node_evo.node.getrawtransaction(proregtx_hash, True)

        # CDeterministicMNState::ToJson() <- CDeterministicMN::pdmnState <- masternode_status
        masternode_status = self.node_evo.node.masternode('status')

        # Generate deprecation-disabled response to avoid having to re-create a masternode again later on
        self.restart_node(self.node_evo.node_idx, extra_args=self.node_evo.node.extra_args +
                          [f'-masternodeblsprivkey={self.node_evo.operator_sk}', '-deprecatedrpc=service'])
        self.connect_nodes(self.node_evo.node_idx, self.node_simple.index) # Needed as restarts don't reconnect nodes
        masternode_status_depr = self.node_evo.node.masternode('status')

        # Stop actively running the masternode so we can issue a CProUpServTx (and enable the deprecation)
        self.restart_node(self.node_evo.node_idx, extra_args=self.node_evo.node.extra_args)
        self.connect_nodes(self.node_evo.node_idx, self.node_simple.index) # Needed as restarts don't reconnect nodes

        # CProUpServTx::ToJson() <- TxToUniv() <- TxToJSON() <- getrawtransaction
        proupservtx_hash = self.node_evo.update_mn(self, f"127.0.0.1:{self.node_evo.port_p2p}", "22200", "22201")
        proupservtx_rpc = self.node_evo.node.getrawtransaction(proupservtx_hash, True)

        # CSimplifiedMNListEntry::ToJson() <- CSimplifiedMNListDiff::mnList <- CSimplifiedMNListDiff::ToJson() <- protx_diff
        masternode_active_height: int = masternode_status['dmnState']['registeredHeight']
        protx_diff_rpc = self.node_evo.node.protx('diff', masternode_active_height - 1, masternode_active_height)

        # CDeterministicMNStateDiff::ToJson() <- CDeterministicMNListDiff::updatedMns <- protx_listdiff
        proupservtx_height = proupservtx_rpc['height']
        protx_listdiff_rpc = self.node_evo.node.protx('listdiff', proupservtx_height - 1, proupservtx_height)

        self.log.info("Test RPCs return an 'addresses' field")
        assert "addresses" in proregtx_rpc['proRegTx'].keys()
        assert "addresses" in masternode_status['dmnState'].keys()
        assert "addresses" in proupservtx_rpc['proUpServTx'].keys()
        assert "addresses" in protx_diff_rpc['mnList'][0].keys()
        assert "addresses" in protx_listdiff_rpc['updatedMNs'][0][proregtx_hash].keys()

        self.log.info("Test 'addresses' report correctly")
        self.check_netinfo_fields(proregtx_rpc['proRegTx']['addresses'], self.node_evo.port_p2p)
        self.check_netinfo_fields(masternode_status['dmnState']['addresses'], self.node_evo.port_p2p)
        self.check_netinfo_fields(proupservtx_rpc['proUpServTx']['addresses'], self.node_evo.port_p2p)
        self.check_netinfo_fields(protx_diff_rpc['mnList'][0]['addresses'], self.node_evo.port_p2p)
        self.check_netinfo_fields(protx_listdiff_rpc['updatedMNs'][0][proregtx_hash]['addresses'], self.node_evo.port_p2p)

        self.log.info("Test RPCs by default no longer return a 'service' field")
        assert "service" not in proregtx_rpc['proRegTx'].keys()
        assert "service" not in masternode_status['dmnState'].keys()
        assert "service" not in proupservtx_rpc['proUpServTx'].keys()
        assert "service" not in protx_diff_rpc['mnList'][0].keys()
        assert "service" not in protx_listdiff_rpc['updatedMNs'][0][proregtx_hash].keys()
        # "service" in "masternode status" is exempt from the deprecation as the primary address is
        # relevant on the host node as opposed to expressing payload information in most other RPCs.
        assert "service" in masternode_status.keys()

        self.node_evo.destroy_mn(self) # Shut down previous masternode
        self.connect_nodes(self.node_evo.node_idx, self.node_simple.index) # Needed as restarts don't reconnect nodes

        self.log.info("Collect RPC responses from node with -deprecatedrpc=service")

        # Re-use chain activity from earlier
        proregtx_rpc = self.node_simple.getrawtransaction(proregtx_hash, True)
        proupservtx_rpc = self.node_simple.getrawtransaction(proupservtx_hash, True)
        protx_diff_rpc = self.node_simple.protx('diff', masternode_active_height - 1, masternode_active_height)
        masternode_status = masternode_status_depr # Pull in response generated from earlier
        protx_listdiff_rpc = self.node_simple.protx('listdiff', proupservtx_height - 1, proupservtx_height)

        self.log.info("Test RPCs return 'service' with -deprecatedrpc=service")
        assert "service" in proregtx_rpc['proRegTx'].keys()
        assert "service" in masternode_status['dmnState'].keys()
        assert "service" in proupservtx_rpc['proUpServTx'].keys()
        assert "service" in protx_diff_rpc['mnList'][0].keys()
        assert "service" in protx_listdiff_rpc['updatedMNs'][0][proregtx_hash].keys()

if __name__ == "__main__":
    NetInfoTest().main()
