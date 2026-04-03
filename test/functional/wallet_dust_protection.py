#!/usr/bin/env python3
# Copyright (c) 2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test -dustprotectionthreshold CLI option.

Verify that UTXOs from external transactions at or below the threshold
are automatically locked to protect against dust attacks.
"""
from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)

# 1 DASH = 100_000_000 duffs
DUFFS = Decimal('0.00000001')


class WalletDustProtectionTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        # node0: sender (no dust protection)
        # node1: receiver with dust protection at 10000 duffs
        # node2: multi-wallet node with dust protection
        # node3: receiver with no dust protection (threshold=0, the default)
        self.extra_args = [
            ["-dustrelayfee=0"],
            ["-dustrelayfee=0", "-dustprotectionthreshold=10000"],
            ["-dustrelayfee=0", "-dustprotectionthreshold=10000", "-nowallet"],
            ["-dustrelayfee=0"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Generate coins for the sender (node0)")
        self.generate(self.nodes[0], COINBASE_MATURITY + 10)
        self.sync_all()

        self.test_external_dust_locked()
        self.test_self_send_not_locked()
        self.test_above_threshold_not_locked()
        self.test_disabled_threshold()
        self.test_existing_utxos_locked_on_restart()
        self.test_multi_wallet()
        self.test_invalid_args()

    def test_external_dust_locked(self):
        """External dust at or below threshold should be locked automatically."""
        self.log.info("Test: external dust gets locked")
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        addr = node1.getnewaddress()

        # Send exactly 10000 duffs (at threshold)
        txid = node0.sendtoaddress(addr, 10000 * DUFFS)
        self.sync_mempools()

        # Should be locked immediately (before confirmation)
        locked = node1.listlockunspent()
        assert_equal(len(locked), 1)
        assert_equal(locked[0]['txid'], txid)

        # Confirm and verify still locked
        self.generate(self.nodes[0], 1)
        self.sync_all()
        locked = node1.listlockunspent()
        assert_equal(len(locked), 1)
        assert_equal(locked[0]['txid'], txid)

        # Cleanup: unlock for further tests
        node1.lockunspent(True, locked)

    def test_self_send_not_locked(self):
        """Self-sends should NOT be locked even if below threshold."""
        self.log.info("Test: self-send dust is not locked")
        node1 = self.nodes[1]

        # Fund node1 with a larger amount first
        addr_fund = node1.getnewaddress()
        self.nodes[0].sendtoaddress(addr_fund, 1)
        self.generate(self.nodes[0], 1)
        self.sync_all()

        # Unlock everything so node1 can spend
        locked = node1.listlockunspent()
        if locked:
            node1.lockunspent(True, locked)

        # Self-send a dust amount
        addr_self = node1.getnewaddress()
        node1.sendtoaddress(addr_self, 5000 * DUFFS)
        self.sync_mempools()

        # Self-send should not create any new locks
        locked = node1.listlockunspent()
        assert_equal(len(locked), 0)

        self.generate(self.nodes[0], 1)
        self.sync_all()

    def test_above_threshold_not_locked(self):
        """UTXOs above the threshold should NOT be locked."""
        self.log.info("Test: above-threshold UTXO is not locked")
        node1 = self.nodes[1]

        # Clear any existing locks
        locked = node1.listlockunspent()
        if locked:
            node1.lockunspent(True, locked)

        addr = node1.getnewaddress()
        # Send 10001 duffs (just above 10000 threshold)
        self.nodes[0].sendtoaddress(addr, 10001 * DUFFS)
        self.sync_mempools()

        locked = node1.listlockunspent()
        assert_equal(len(locked), 0)

        self.generate(self.nodes[0], 1)
        self.sync_all()

    def test_disabled_threshold(self):
        """With default threshold (0), nothing should be locked."""
        self.log.info("Test: threshold=0 disables dust protection")
        node3 = self.nodes[3]

        addr = node3.getnewaddress()
        self.nodes[0].sendtoaddress(addr, 5000 * DUFFS)
        self.sync_mempools()

        locked = node3.listlockunspent()
        assert_equal(len(locked), 0)

        self.generate(self.nodes[0], 1)
        self.sync_all()

    def test_existing_utxos_locked_on_restart(self):
        """Pre-existing dust UTXOs should be locked when node starts with -dustprotectionthreshold."""
        self.log.info("Test: existing UTXOs locked on restart")
        node3 = self.nodes[3]  # no dust protection

        # Send dust to node3 while protection is off
        addr = node3.getnewaddress()
        self.nodes[0].sendtoaddress(addr, 8000 * DUFFS)
        self.generate(self.nodes[0], 1)
        self.sync_all()

        assert_equal(len(node3.listlockunspent()), 0)

        # Capture exact dust outpoints before restart
        THRESHOLD = 10000
        expected_outpoints = set()
        for utxo in node3.listunspent():
            if utxo['amount'] <= THRESHOLD * DUFFS:
                expected_outpoints.add((utxo['txid'], utxo['vout']))
        assert len(expected_outpoints) > 0, "Test requires at least one dust UTXO on node3"

        # Restart node3 WITH dust protection — all existing dust should get locked
        self.restart_node(3, ["-dustrelayfee=0", "-dustprotectionthreshold=%d" % THRESHOLD])
        self.connect_nodes(0, 3)

        locked = node3.listlockunspent()
        locked_outpoints = {(entry['txid'], entry['vout']) for entry in locked}
        assert_equal(locked_outpoints, expected_outpoints)

        # Restart again WITHOUT protection — locks should persist (written to DB)
        self.restart_node(3, ["-dustrelayfee=0"])
        self.connect_nodes(0, 3)

        locked = node3.listlockunspent()
        locked_outpoints = {(entry['txid'], entry['vout']) for entry in locked}
        assert_equal(locked_outpoints, expected_outpoints)

        # Cleanup
        node3.lockunspent(True, locked)

    def test_multi_wallet(self):
        """Dust protection should work across multiple wallets on the same node."""
        self.log.info("Test: multi-wallet dust protection")
        node2 = self.nodes[2]

        # Create two wallets on node2
        node2.createwallet(wallet_name='wallet_a')
        node2.createwallet(wallet_name='wallet_b')
        wallet_a = node2.get_wallet_rpc('wallet_a')
        wallet_b = node2.get_wallet_rpc('wallet_b')

        addr_a = wallet_a.getnewaddress()
        addr_b = wallet_b.getnewaddress()

        # Send dust to both wallets
        self.nodes[0].sendtoaddress(addr_a, 5000 * DUFFS)
        self.nodes[0].sendtoaddress(addr_b, 7000 * DUFFS)
        self.generate(self.nodes[0], 1)
        self.sync_all()

        # Both wallets should have their dust locked
        locked_a = wallet_a.listlockunspent()
        locked_b = wallet_b.listlockunspent()
        assert_equal(len(locked_a), 1)
        assert_equal(len(locked_b), 1)

        # Send an above-threshold amount — should NOT be locked
        addr_a2 = wallet_a.getnewaddress()
        self.nodes[0].sendtoaddress(addr_a2, 20000 * DUFFS)
        self.generate(self.nodes[0], 1)
        self.sync_all()

        # wallet_a still has only 1 locked UTXO (the dust one)
        locked_a = wallet_a.listlockunspent()
        assert_equal(len(locked_a), 1)

        # Restart and verify locks persist across wallets
        self.restart_node(2, ["-dustrelayfee=0", "-dustprotectionthreshold=10000",
                              "-wallet=wallet_a", "-wallet=wallet_b"])
        self.connect_nodes(0, 2)
        wallet_a = node2.get_wallet_rpc('wallet_a')
        wallet_b = node2.get_wallet_rpc('wallet_b')

        locked_a = wallet_a.listlockunspent()
        locked_b = wallet_b.listlockunspent()
        assert_equal(len(locked_a), 1)
        assert_equal(len(locked_b), 1)

    def test_invalid_args(self):
        """Invalid -dustprotectionthreshold values should be rejected."""
        self.log.info("Test: invalid CLI args rejected")

        # Negative value
        self.stop_node(3)
        self.nodes[3].assert_start_raises_init_error(
            ["-dustprotectionthreshold=-1"],
            "Error: Invalid value for -dustprotectionthreshold: must be >= 0",
        )

        # Above maximum (1000000)
        self.nodes[3].assert_start_raises_init_error(
            ["-dustprotectionthreshold=1000001"],
            "Error: Invalid value for -dustprotectionthreshold: exceeds maximum (1000000)",
        )

        # Restart node3 normally for clean state
        self.start_node(3, ["-dustrelayfee=0"])


if __name__ == '__main__':
    WalletDustProtectionTest().main()
