#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test CoinJoin client restart and reset handling.

Runs real CoinJoin client sessions on regtest long enough to exercise
queue/session startup, then restarts one participant while a session is active.

Covered:
- use of confirmed collaterals and denominated outputs
- queue/session startup against real masternodes
- wallet recovery when a participant restarts during an active session
- coinjoin start/stop/reset while sessions are live and after failures
- client-side mixing being unavailable on masternodes

Mixing timeouts (30s queue, 15s signing) are driven by mocktime. This test is
intentionally narrower than full anonymization coverage because it targets the
CoinJoin client lifetime path fixed by #7259.
"""

from decimal import Decimal

from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

# Keep the mixing target small so session startup uses only the prefunded
# denominations below.
MIXING_AMOUNT_TARGET = 2
# The protocol minimum.
MIXING_ROUNDS_TARGET = 2

PREFUNDED_DENOM = Decimal("1.00001000")
PREFUNDED_COLLATERAL = Decimal("0.00020000")
PREFUNDED_DENOM_OUTPUTS = 8
PREFUNDED_COLLATERAL_OUTPUTS = 8

MASTERNODES = 6
BACKUP_EXISTS_WARNING = ("Warning: Failed to create backup, file already exists! This could happen if you restarted "
                         "wallet in less than 60 seconds. You can continue if you are ok with this.")


class WalletCoinJoinMixingTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # Node 0: controller/miner, nodes 1-2: mixing wallets, remaining nodes: masternodes.
        # The framework disables automatic wallet backups, but CoinJoin
        # refuses to mix legacy wallets without them, so re-enable them only
        # for the mixing wallets. Keep one session per wallet so the small
        # regtest masternode set is not consumed by too many parallel attempts.
        mixing_args = [
            "-debug=coinjoin",
            "-createwalletbackups=1",
            "-keypool=400",
            "-coinjoinsessions=1",
        ]
        extra_args = [
            ["-debug=coinjoin"],
            mixing_args,
            mixing_args,
        ] + [["-debug=coinjoin"]] * MASTERNODES
        self.set_dash_test_params(3 + MASTERNODES, MASTERNODES, extra_args=extra_args)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.w1 = self.nodes[1]
        self.w2 = self.nodes[2]
        self.wallets = [self.w1, self.w2]

        # Observability collected while pumping the mixing loop
        self.session_masternodes = set()
        self.session_states = set()
        self.max_queue_size = 0

        self.prepare_chain()
        self.test_mixing_unavailable_on_masternodes()
        self.fund_mixing_wallets()
        self.test_active_session_restart_recovery()
        self.verify_sessions_and_queues()
        self.test_stop_and_reset()
        self.stop_wallet_node_allowing_recent_backup(1)
        self.stop_wallet_node_allowing_recent_backup(2)

    def pump_mixing(self, wallets=None):
        """Advance one mixing 'tick'.

        Bumps mocktime (which drives client/server maintenance and the 30s
        queue / 15s signing timeouts), records session/queue state for later
        assertions, captures mixing transactions from the mempool and
        confirms whatever is pending so follow-up sessions can start.
        """
        running_nodes = self.running_nodes()
        self.bump_mocktime(3, nodes=running_nodes)

        wallets = self.wallets if wallets is None else wallets
        for wallet in wallets:
            info = wallet.getcoinjoininfo()
            self.max_queue_size = max(self.max_queue_size, info['queue_size'])
            for session in info['sessions']:
                if 'protxhash' in session:
                    self.session_masternodes.add(session['protxhash'])
                self.session_states.add(session['state'])

        node = self.nodes[0]
        mempool = node.getrawmempool()
        if mempool:
            self.generate(node, 1, sync_fun=lambda: self.sync_blocks(running_nodes))

    def running_nodes(self):
        return [node for node in self.nodes if node.running]

    def settle_mempool(self):
        self.sync_mempools([node for node in [self.nodes[0], self.w1, self.w2] if node.running])
        if self.nodes[0].getrawmempool():
            running_nodes = self.running_nodes()
            self.bump_mocktime(1, nodes=running_nodes)
            self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(running_nodes))

    def stop_and_reset_mixing(self, wallets=None):
        wallets = self.wallets if wallets is None else wallets
        for wallet in wallets:
            if wallet.getcoinjoininfo()['running']:
                wallet.coinjoin('stop')
            assert_equal(wallet.coinjoin('reset'), "Mixing was reset")
            assert_equal(wallet.getcoinjoininfo()['sessions'], [])
            assert_equal(wallet.listlockunspent(), [])

    def stop_wallet_node_allowing_recent_backup(self, index):
        node = self.nodes[index]
        if not node.running:
            return
        node.log.debug("Stopping node")
        node.stop()
        node.stderr.seek(0)
        stderr = node.stderr.read().decode('utf-8').strip()
        assert stderr in ("", BACKUP_EXISTS_WARNING), f"Unexpected stderr {stderr}"
        node.stdout.close()
        node.stderr.close()
        del node.p2ps[:]
        node.wait_until_stopped()

    def restart_wallet_node_allowing_recent_backup(self, index):
        self.stop_wallet_node_allowing_recent_backup(index)
        self.start_node(index)

    def prepare_chain(self):
        # There are no InstantSend quorums here, but the framework enables the
        # InstantSend spork by default, which makes the miner hold back
        # non-locked transactions for 10 minutes. CoinJoin does not need
        # InstantSend, so turn it off.
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        self.log.info("Make sure every masternode has been paid at least once")

        # CCoinJoinClientSession skips masternodes that are next in the
        # payment queue, which on regtest means masternodes that have never
        # been paid would never be picked for mixing.
        def all_masternodes_paid():
            if all(h > 0 for h in self.nodes[0].masternodelist("lastpaidblock").values()):
                return True
            self.bump_mocktime(1)
            self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks())
            return False
        self.wait_until(all_masternodes_paid, timeout=60)

    def configure_mixing(self):
        for wallet in self.wallets:
            wallet.setcoinjoinamount(MIXING_AMOUNT_TARGET)
            wallet.setcoinjoinrounds(MIXING_ROUNDS_TARGET)

    def wait_for_joiner_visible_queue(self):
        connected_mns = set()

        def connect_joiner_to_starter_mn():
            sessions = self.w1.getcoinjoininfo()['sessions']
            if not sessions:
                return False

            protxhash = sessions[0].get('protxhash')
            if protxhash is None:
                return False
            if protxhash in connected_mns:
                return True

            mn = self.get_mninfo(protxhash)
            assert mn is not None and mn.nodeIdx is not None, f"unknown mixing masternode {protxhash}"
            self.connect_nodes(2, mn.nodeIdx)
            connected_mns.add(protxhash)
            return True

        self.wait_until(lambda: connect_joiner_to_starter_mn() or (self.pump_mixing() and False),
                        timeout=120, sleep=0.25)

        def queue_seen_by_joiner():
            connect_joiner_to_starter_mn()
            if self.w2.getcoinjoininfo()['queue_size'] > 0:
                return True
            self.pump_mixing()
            return False
        self.wait_until(queue_seen_by_joiner, timeout=120, sleep=0.25)

    def fund_mixing_wallets(self):
        self.log.info("Fund the mixing wallets with confirmed denominations and collaterals")
        for wallet in self.wallets:
            for _ in range(PREFUNDED_DENOM_OUTPUTS):
                self.nodes[0].sendtoaddress(wallet.getnewaddress(), PREFUNDED_DENOM)
            for _ in range(PREFUNDED_COLLATERAL_OUTPUTS):
                self.nodes[0].sendtoaddress(wallet.getnewaddress(), PREFUNDED_COLLATERAL)
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks())
        for wallet in self.wallets:
            utxos = wallet.listunspent()
            assert_greater_than(sum(1 for utxo in utxos if utxo['amount'] == PREFUNDED_DENOM),
                                PREFUNDED_DENOM_OUTPUTS - 1)
            assert_greater_than(sum(1 for utxo in utxos if utxo['amount'] == PREFUNDED_COLLATERAL),
                                PREFUNDED_COLLATERAL_OUTPUTS - 1)

    def test_active_session_restart_recovery(self):
        self.log.info("Recover after a participant restarts during active mixing")
        self.configure_mixing()

        assert_equal(self.w1.coinjoin('start'), "Mixing requested")

        def queue_started():
            if self.w1.getcoinjoininfo()['queue_size'] > 0:
                return True
            self.pump_mixing()
            return False

        self.wait_until(queue_started, timeout=120, sleep=0.25)
        assert_greater_than(self.w1.getcoinjoininfo()['queue_size'], 0)
        self.wait_for_joiner_visible_queue()

        assert_equal(self.w2.coinjoin('start'), "Mixing requested")

        def participants_joined_same_session():
            sessions = [wallet.getcoinjoininfo()['sessions'] for wallet in self.wallets]
            if all(sessions):
                protxhashes = [wallet_sessions[0].get('protxhash') for wallet_sessions in sessions]
                if all(protxhash is not None for protxhash in protxhashes) and len(set(protxhashes)) == 1:
                    return True
            self.pump_mixing()
            return False

        self.wait_until(participants_joined_same_session, timeout=120, sleep=0.25)

        # Restart a live participant to guard the CoinJoin wallet-manager
        # lifetime cleanup while another wallet remains in the same session.
        self.stop_wallet_node_allowing_recent_backup(2)
        self.wallets = [self.w1]

        def survivor_released_session():
            self.pump_mixing(wallets=[self.w1])
            info = self.w1.getcoinjoininfo()
            return not info['sessions'] or any(session['state'] == 'ERROR' for session in info['sessions'])

        self.wait_until(survivor_released_session, timeout=120, sleep=0.25)
        # Reset the survivor before restarting it so cleanup is attributable
        # to the RPC rather than process restart.
        self.stop_and_reset_mixing([self.w1])

        self.start_node(2)
        self.w2 = self.nodes[2]
        self.wallets = [self.w1, self.w2]
        self.connect_nodes(2, 0)
        self.sync_blocks()
        self.stop_and_reset_mixing([self.w2])

        self.restart_wallet_node_allowing_recent_backup(1)
        self.restart_wallet_node_allowing_recent_backup(2)
        self.w1 = self.nodes[1]
        self.w2 = self.nodes[2]
        self.wallets = [self.w1, self.w2]
        self.connect_nodes(1, 0)
        self.connect_nodes(2, 0)
        self.sync_blocks()
        self.settle_mempool()
        self.bump_mocktime(60)
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks())

    def test_mixing_unavailable_on_masternodes(self):
        self.log.info("Client-side mixing must not be available on masternodes")
        mn_node = self.mninfo[0].get_node(self)
        # Masternodes cannot run with a wallet at all (-masternodeblsprivkey
        # force-disables it), so client-side mixing RPCs are not even
        # registered on them ...
        assert_raises_rpc_error(-32601, "Method not found", mn_node.coinjoin, 'start')
        # ... and getcoinjoininfo reports the server pool, not a client
        mn_info = mn_node.getcoinjoininfo()
        assert 'running' not in mn_info
        assert 'state' in mn_info

    def verify_sessions_and_queues(self):
        self.log.info("Verify observed sessions ran on our masternodes")
        assert_greater_than(len(self.session_masternodes), 0)
        registered = {mn.proTxHash for mn in self.mninfo}
        assert self.session_masternodes.issubset(registered), \
            f"unexpected mixing masternodes: {self.session_masternodes - registered}"
        self.log.info(f"Observed sessions on {len(self.session_masternodes)} masternode(s), "
                      f"states {sorted(self.session_states)}, max queue size {self.max_queue_size}")
        assert_greater_than(self.max_queue_size, 0)

    def test_stop_and_reset(self):
        self.log.info("Stop mixing and reset the clients")
        self.stop_and_reset_mixing()
        for wallet in self.wallets:
            info = wallet.getcoinjoininfo()
            assert_equal(info['enabled'], True)
            assert_equal(info['running'], False)

        # Confirm anything still in flight: the anonymized balance counts
        # unconfirmed outputs while listunspent does not, so the balance
        # checks below need a settled mempool.
        self.settle_mempool()


if __name__ == '__main__':
    WalletCoinJoinMixingTest().main()
