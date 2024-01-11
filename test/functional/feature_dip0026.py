#!/usr/bin/env python3

'''
feature_dip0026.py

Functional test DIP0026

'''
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal


class DIP0026Test(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(6, 5, fast_dip3_enforcement=True, evo_count=1)

    def run_test(self):
        multi_payee_mns = []
        for i in range(len(self.nodes)):
            if i != 0:
                self.connect_nodes(i, 0)
        self.activate_dip8()

        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        # activate v19, v20
        self.activate_v19(expected_activation_height=900)
        self.log.info("Activated v19 at height:" + str(self.nodes[0].getblockcount()))

        self.activate_v20()
        self.log.info("Activated v20 at height:" + str(self.nodes[0].getblockcount()))

        # make sure that there is a quorum that can handle instant send
        self.mine_cycle_quorum(llmq_type_name='llmq_test_dip0024', llmq_type=103)

        self.log.info("Checking that multipayee masternodes are rejected before dip0026 activation")
        self.dynamically_add_masternode(evo=False, rnd=7, n_payees=2, should_be_rejected=True)

        # activate dip0026
        self.activate_dip0026()
        self.log.info("Activated dip0026 at height:" + str(self.nodes[0].getblockcount()))
        self.log.info("Creating a 2-payees masternode")
        multi_payee_mns.append(self.dynamically_add_masternode(evo=False, rnd=7, n_payees=2))

        self.log.info("Checking that there cannot be more than 32 payees:")
        self.dynamically_add_masternode(evo=False, rnd=8, n_payees=33, should_be_rejected=True)

        self.log.info("Creating an evo 3-payees masternode")
        multi_payee_mns.append(self.dynamically_add_masternode(evo=False, rnd=8, n_payees=3))

        self.log.info("Checking masternode payments:")
        self.test_multipayee_payment(multi_payee_mns)

    def test_multipayee_payment(self, multi_payee_mns):
        multi_payee_reward_counts = [0, 0]
        # with two full cycles both multi payee masternodes have to be paid twice
        for _ in range(len(self.mninfo)*2):
            self.nodes[0].generate(1)
            # make sure every node has the same chain tip
            self.sync_all()
            # find out who won with a RPC call
            rpc_masternode_winner_info = self.nodes[0].masternode("payments")[0]["masternodes"][0]
            # for each multi payee masternode that we created check if it has been paid in the last block
            for i, multi_payee_mn in enumerate(multi_payee_mns):
                if multi_payee_mn.proTxHash == rpc_masternode_winner_info["proTxHash"]:
                    multi_payee_reward_counts[i] += 1
                    # if so verify that the right addresses/amount have been paid
                    # NB: Skip the first rpc entry since it's the coinbase tx
                    self.test_multipayee_payment_internal(rpc_masternode_winner_info["payees"][1:], multi_payee_mn)

        # each multi payee mn must have been paid twice
        for count in multi_payee_reward_counts:
            assert_equal(count, 2)

    def test_multipayee_payment_internal(self, rpc_masternode_winner_payees, multi_payee_mn):
        tot_paid = 0
        # 1) Verify correctness of paid addresses
        for i, payee_share in enumerate(multi_payee_mn.rewards_address):
            payee_address = payee_share[0]
            assert_equal(rpc_masternode_winner_payees[i]['address'], payee_address)
            tot_paid += rpc_masternode_winner_payees[i]['amount']
        # 2) Verify correctness of rewards
        for i, payee_share in enumerate(multi_payee_mn.rewards_address):
            # this is not the exact formula with which rewards are calculated:
            # due to integer division it differs up to (10k * number_of_payees) satoshi,
            # but for the sake of the test it's fine
            assert (abs(tot_paid * payee_share[1] - rpc_masternode_winner_payees[i]['amount'] * 10000) < 10000 * len(multi_payee_mn.rewards_address))


if __name__ == '__main__':
    DIP0026Test().main()
