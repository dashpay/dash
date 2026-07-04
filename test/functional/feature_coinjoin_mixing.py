#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test end-to-end CoinJoin mixing.

Runs real mixing sessions on regtest: two wallet nodes mix through
masternodes until funds are fully anonymized, then spend the mixed funds.

Covered:
- use of confirmed collaterals and denominated outputs
- full dsa -> dsq -> dsi -> dss -> dstx session flow against real masternodes
  (regtest sessions start once 2 participants joined and the queue timed out)
- structure of the resulting mixing transactions (uniform denomination,
  #inputs == #outputs, zero fee) and that each one is a joint transaction
  between both participating wallets
- round bookkeeping (listunspent coinjoin_rounds) and anonymized balance
  reporting (getbalances().mine.coinjoin, getcoinjoininfo)
- spending fully mixed funds with sendtoaddress use_cj=true
- coinjoin start/stop/reset while sessions are live
- client-side mixing being unavailable on masternodes

Mixing timeouts (30s queue, 15s signing) are driven by mocktime; the test
pumps the mock clock and confirms pending transactions until mixing
completes.
"""

from decimal import Decimal

from test_framework.messages import COIN
from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

# Standard denominations, in duffs (see src/coinjoin/common.h)
DENOMINATIONS = [
    1000010000,  # 10.0001
    100001000,   # 1.0001
    10000100,    # 0.1001
    1000010,     # 0.01001
    100001,      # 0.001001
]
DENOM_AMOUNTS = {Decimal(d) / COIN for d in DENOMINATIONS}

# Keep the mixing target small so that only the smaller denominations are
# needed and a handful of successful sessions completes the test.
MIXING_AMOUNT_TARGET = 2
# The protocol minimum, to finish mixing in as few sessions as possible.
MIXING_ROUNDS_TARGET = 2

PREFUNDED_DENOM = Decimal("1.00001000")
PREFUNDED_COLLATERAL = Decimal("0.00020000")
PREFUNDED_DENOM_OUTPUTS = 8
PREFUNDED_COLLATERAL_OUTPUTS = 8
PREFUNDED_BALANCE = PREFUNDED_DENOM * PREFUNDED_DENOM_OUTPUTS + PREFUNDED_COLLATERAL * PREFUNDED_COLLATERAL_OUTPUTS

MASTERNODES = 6


class CoinJoinMixingTest(DashTestFramework):
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
            "-coinjoinrandomrounds=0",
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
        self.dstx = {}  # txid -> decoded tx
        self.session_masternodes = set()
        self.session_states = set()
        self.max_queue_size = 0

        self.prepare_chain()
        self.test_mixing_unavailable_on_masternodes()
        self.start_mixing()
        self.wait_for_denominations()
        self.wait_for_anonymized_balance()
        self.verify_sessions_and_queues()
        self.verify_mixing_transactions()
        self.test_stop_and_reset()
        self.verify_rounds_and_balances()
        self.spend_mixed_funds()
        # Automatic backup names are precise to the minute. Move mocktime far
        # enough before framework shutdown so any final backup attempt cannot
        # collide with the start-of-test backup filename.
        self.bump_mocktime(60)

    def pump_mixing(self):
        """Advance one mixing 'tick'.

        Bumps mocktime (which drives client/server maintenance and the 30s
        queue / 15s signing timeouts), records session/queue state for later
        assertions, captures mixing transactions from the mempool and
        confirms whatever is pending so follow-up sessions can start.
        """
        self.bump_mocktime(3)

        for wallet in self.wallets:
            info = wallet.getcoinjoininfo()
            self.max_queue_size = max(self.max_queue_size, info['queue_size'])
            for session in info['sessions']:
                if 'protxhash' in session:
                    self.session_masternodes.add(session['protxhash'])
                self.session_states.add(session['state'])

        # Capture mixing transactions before confirming them. The final
        # transaction of a session is the only zero-fee transaction CoinJoin
        # produces (denomination/collateral creation transactions all pay
        # normal fees).
        node = self.nodes[0]
        mempool = node.getrawmempool()
        for txid in mempool:
            if txid in self.dstx:
                continue
            if node.getmempoolentry(txid)['fees']['base'] == 0:
                self.dstx[txid] = node.getrawtransaction(txid, True)

        if mempool:
            self.generate(node, 1, sync_fun=lambda: self.sync_blocks())

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

        self.log.info("Fund the mixing wallets with confirmed denominations and collaterals")
        for wallet in self.wallets:
            for _ in range(PREFUNDED_DENOM_OUTPUTS):
                self.nodes[0].sendtoaddress(wallet.getnewaddress(), PREFUNDED_DENOM)
            for _ in range(PREFUNDED_COLLATERAL_OUTPUTS):
                self.nodes[0].sendtoaddress(wallet.getnewaddress(), PREFUNDED_COLLATERAL)
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks())
        for wallet in self.wallets:
            assert_equal(wallet.getbalance(), PREFUNDED_BALANCE)

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

    def start_mixing(self):
        self.log.info("Configure and start mixing on both wallets")
        for wallet in self.wallets:
            wallet.setcoinjoinamount(MIXING_AMOUNT_TARGET)
            wallet.setcoinjoinrounds(MIXING_ROUNDS_TARGET)

        # Start one wallet first so it advertises a queue for the second
        # wallet to join instead of both wallets racing to create separate
        # sessions.
        assert_equal(self.w1.coinjoin('start'), "Mixing requested")
        for _ in range(5):
            if any(wallet.getcoinjoininfo()['queue_size'] > 0 for wallet in self.wallets):
                break
            self.pump_mixing()
        assert_equal(self.w2.coinjoin('start'), "Mixing requested")

        for wallet in self.wallets:
            info = wallet.getcoinjoininfo()
            assert_equal(info['enabled'], True)
            assert_equal(info['running'], True)
            assert_equal(info['max_amount'], MIXING_AMOUNT_TARGET)
            assert_equal(info['max_rounds'], MIXING_ROUNDS_TARGET)

    def wait_for_denominations(self):
        self.log.info("Wait for collaterals and denominated outputs to be created")

        def has_denominations(wallet):
            return any(utxo['amount'] in DENOM_AMOUNTS for utxo in wallet.listunspent())
        for wallet in self.wallets:
            self.wait_until(lambda: has_denominations(wallet) or (self.pump_mixing() and False), timeout=120)

    def wait_for_anonymized_balance(self):
        self.log.info("Mix until both wallets report an anonymized balance")
        restarted_after_first_round = False

        def both_have_round_one_outputs():
            return all(any(utxo['coinjoin_rounds'] >= 1 for utxo in wallet.listunspent())
                       for wallet in self.wallets)

        def both_anonymized():
            nonlocal restarted_after_first_round
            if all(w.getbalances()['mine']['coinjoin'] > 0 for w in self.wallets):
                return True
            self.pump_mixing()
            if not restarted_after_first_round and both_have_round_one_outputs():
                self.restart_mixing_wallets()
                restarted_after_first_round = True
            return False
        self.wait_until(both_anonymized, timeout=600, sleep=0.25)

        for wallet in self.wallets:
            # status must be reportable while mixing
            wallet.coinjoin('status')

    def restart_mixing_wallets(self):
        self.log.info("Restart mixing wallets before the second round")
        for wallet in self.wallets:
            if wallet.getcoinjoininfo()['running']:
                wallet.coinjoin('stop')

        # Clear masternode connection bookkeeping in the tiny regtest topology
        # while preserving the wallets and their round-one denominated outputs.
        self.bump_mocktime(60)
        self.restart_node(1)
        self.restart_node(2)
        self.w1 = self.nodes[1]
        self.w2 = self.nodes[2]
        self.wallets = [self.w1, self.w2]
        self.connect_nodes(1, 0)
        self.connect_nodes(2, 0)
        self.sync_blocks()
        # Single-session CoinJoin deliberately waits at least one block after
        # a successful session before starting another one.
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks())
        self.start_mixing()

    def verify_sessions_and_queues(self):
        self.log.info("Verify observed sessions ran on our masternodes")
        assert_greater_than(len(self.session_masternodes), 0)
        registered = {mn.proTxHash for mn in self.mninfo}
        assert self.session_masternodes.issubset(registered), \
            f"unexpected mixing masternodes: {self.session_masternodes - registered}"
        self.log.info(f"Observed sessions on {len(self.session_masternodes)} masternode(s), "
                      f"states {sorted(self.session_states)}, max queue size {self.max_queue_size}")
        assert_greater_than(self.max_queue_size, 0)

    def verify_mixing_transactions(self):
        self.log.info("Verify the structure of captured mixing transactions")
        assert_greater_than(len(self.dstx), 0)
        for txid, tx in self.dstx.items():
            # A mixing transaction has as many outputs as inputs and all
            # outputs are of one single denomination.
            assert_equal(len(tx['vin']), len(tx['vout']))
            values = {out['value'] for out in tx['vout']}
            assert_equal(len(values), 1)
            assert values.issubset(DENOM_AMOUNTS), f"non-denominated mixing output in {txid}: {values}"
            # Regtest sessions need 2 participants and each wallet runs at
            # most one session, so every mix is a joint transaction of both
            # wallets.
            for wallet in self.wallets:
                wallet_tx = wallet.gettransaction(txid)  # throws if unknown to the wallet
                assert_equal(wallet_tx['txid'], txid)
        self.log.info(f"Verified {len(self.dstx)} mixing transaction(s)")

    def verify_rounds_and_balances(self):
        self.log.info("Verify round bookkeeping and balance reporting")
        for wallet in self.wallets:
            mixed = [utxo for utxo in wallet.listunspent() if utxo['coinjoin_rounds'] >= MIXING_ROUNDS_TARGET]
            assert_greater_than(len(mixed), 0)
            for utxo in mixed:
                assert utxo['amount'] in DENOM_AMOUNTS
            balances = wallet.getbalances()['mine']
            assert_equal(balances['coinjoin'], sum(utxo['amount'] for utxo in mixed))
            assert_equal(wallet.getwalletinfo()['coinjoin_balance'], balances['coinjoin'])

    def test_stop_and_reset(self):
        self.log.info("Stop mixing and reset the clients")
        for wallet in self.wallets:
            wallet.coinjoin('stop')
            info = wallet.getcoinjoininfo()
            assert_equal(info['enabled'], True)
            assert_equal(info['running'], False)
            assert_equal(wallet.coinjoin('reset'), "Mixing was reset")

        # Confirm anything still in flight: the anonymized balance counts
        # unconfirmed outputs while listunspent does not, so the balance
        # checks below need a settled mempool.
        self.sync_mempools([self.nodes[0], self.w1, self.w2])
        if self.nodes[0].getrawmempool():
            self.bump_mocktime(1)
            self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks())

    def spend_mixed_funds(self):
        self.log.info("Spend fully mixed funds with use_cj")
        mixed_utxos = {(utxo['txid'], utxo['vout'])
                       for utxo in self.w1.listunspent() if utxo['coinjoin_rounds'] >= MIXING_ROUNDS_TARGET}
        anonymized = self.w1.getbalances()['mine']['coinjoin']
        address = self.nodes[0].getnewaddress()
        txid = self.w1.sendtoaddress(address=address, amount=anonymized, subtractfeefromamount=True, use_cj=True)

        # Only fully mixed inputs may fund this transaction
        tx = self.w1.getrawtransaction(txid, True)
        for txin in tx['vin']:
            assert (txin['txid'], txin['vout']) in mixed_utxos, \
                f"spent a non-mixed input: {txin['txid']}:{txin['vout']}"

        self.sync_mempools([self.nodes[0], self.w1, self.w2])
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks())
        assert_equal(self.w1.getbalances()['mine']['coinjoin'], 0)
        received = self.nodes[0].getreceivedbyaddress(address)
        fee = anonymized - received
        assert_greater_than(received, 0)
        assert_greater_than(Decimal("0.001"), fee)


if __name__ == '__main__':
    CoinJoinMixingTest().main()
