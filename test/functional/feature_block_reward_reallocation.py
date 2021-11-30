#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
from test_framework.blocktools import create_block, create_coinbase, get_masternode_payment
from test_framework.messages import CTxOut, FromHex, CCbTx, CTransaction, ToHex
from test_framework.script import CScript
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, get_bip9_status, hex_str_to_bytes

'''
feature_block_reward_reallocation.py

Checks block reward reallocation correctness

'''


class BlockRewardReallocationTest(DashTestFramework):
    def add_options(self, parser):
        parser.add_argument("--first_half", dest="first_half", default=False, action="store_true",
                            help="Only run the first half of the test.")
        parser.add_argument("--second_half", dest="second_half", default=False, action="store_true",
                            help="Only tun the second half of the test")

    def set_test_params(self):
        self.set_dash_test_params(2, 1, fast_dip3_enforcement=True)
        self.set_dash_dip8_activation(450)

    # 536870912 == 0x20000000, i.e. not signalling for anything
    def create_test_block(self, version=536870912):
        self.bump_mocktime(5, False)
        bt = self.nodes[0].getblocktemplate()
        tip = int(bt['previousblockhash'], 16)
        nextheight = bt['height']

        coinbase = create_coinbase(nextheight)
        coinbase.nVersion = 3
        coinbase.nType = 5 # CbTx
        coinbase.vout[0].nValue = bt['coinbasevalue']
        for mn in bt['masternode']:
            coinbase.vout.append(CTxOut(mn['amount'], CScript(hex_str_to_bytes(mn['script']))))
            coinbase.vout[0].nValue -= mn['amount']
        cbtx = FromHex(CCbTx(), bt['coinbase_payload'])
        coinbase.vExtraPayload = cbtx.serialize()
        coinbase.rehash()
        coinbase.calc_sha256()

        block = create_block(tip, coinbase, self.mocktime)
        block.nVersion = version
        # Add quorum commitments from template
        for tx in bt['transactions']:
            tx2 = FromHex(CTransaction(), tx['data'])
            if tx2.nType == 6:
                block.vtx.append(tx2)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.solve()
        return block

    def mine_blocks(self, num_blocks, expected_realloc=None):
        batch = 15  # generate at most 10 blocks at once
        for i in range((num_blocks - 1) // batch):
            self.bump_mocktime(batch, False)
            self.nodes[0].generate(batch)

        if expected_realloc:
            self.nodes[0].generate((num_blocks - 1) % batch)
            assert_equal(get_bip9_status(self.nodes[0], 'realloc')['status'], expected_realloc)
            self.nodes[0].generate(1)
        else:
            self.nodes[0].generate(num_blocks % batch)

    def signal(self, num_blocks, expected_lockin):
        self.log.info("Signal with %d/500 blocks" % (num_blocks))
        # create and send non-signalling blocks
        for _ in range(500 - num_blocks):
            test_block = self.create_test_block()
            self.nodes[0].submitblock(ToHex(test_block))
        if num_blocks > 0:
            self.mine_blocks(num_blocks, 'started')
        if expected_lockin:
            assert_equal(get_bip9_status(self.nodes[0], 'realloc')['status'], 'locked_in')
        else:
            assert_equal(get_bip9_status(self.nodes[0], 'realloc')['status'], 'started')

    def threshold(self, attempt):
        threshold_calc = 400 - attempt * attempt
        if threshold_calc < 300:
            return 300
        return threshold_calc

    def run_test(self):
        self.log.info("Wait for DIP3 to activate")
        while get_bip9_status(self.nodes[0], 'dip0003')['status'] != 'active':
            self.bump_mocktime(10, False)
            self.nodes[0].generate(10)

        self.log.info("Mine all but one remaining block in the window")
        bi = self.nodes[0].getblockchaininfo()
        self.mine_blocks(498 - bi['blocks'])

        self.log.info("Initial state is DEFINED")
        bi = self.nodes[0].getblockchaininfo()
        assert_equal(bi['blocks'], 498)
        assert_equal(bi['bip9_softforks']['realloc']['status'], 'defined')

        self.log.info("Advance from DEFINED to STARTED at height = 499")
        self.nodes[0].generate(1)
        bi = self.nodes[0].getblockchaininfo()
        assert_equal(bi['blocks'], 499)
        assert_equal(bi['bip9_softforks']['realloc']['status'], 'started')
        assert_equal(bi['bip9_softforks']['realloc']['statistics']['threshold'], self.threshold(0))

        self.signal(399, False)  # 1 block short

        self.log.info("Still STARTED but new threshold should be lower at height = 999")
        bi = self.nodes[0].getblockchaininfo()
        assert_equal(bi['blocks'], 999)
        assert_equal(bi['bip9_softforks']['realloc']['statistics']['threshold'], self.threshold(1))

        self.signal(398, False)  # 1 block short again

        self.log.info("Still STARTED but new threshold should be even lower at height = 1499")
        bi = self.nodes[0].getblockchaininfo()
        assert_equal(bi['blocks'], 1499)
        assert_equal(bi['bip9_softforks']['realloc']['statistics']['threshold'], self.threshold(2))
        pre_locked_in_blockhash = bi['bestblockhash']

        if self.options.first_half or (not self.options.first_half and not self.options.second_half):

            self.signal(396, True)  # just enough to lock in
            self.log.info("Advanced to LOCKED_IN at height = 1999")

            for i in range(49):
                self.nodes[0].generate(10)
            self.nodes[0].generate(9)

            self.log.info("Still LOCKED_IN at height = 2498")
            bi = self.nodes[0].getblockchaininfo()
            assert_equal(bi['blocks'], 2498)
            assert_equal(bi['bip9_softforks']['realloc']['status'], 'locked_in')

            self.log.info("Advance from LOCKED_IN to ACTIVE at height = 2499")
            self.nodes[0].generate(1)  # activation
            bi = self.nodes[0].getblockchaininfo()
            assert_equal(bi['blocks'], 2499)
            assert_equal(bi['bip9_softforks']['realloc']['status'], 'active')
            assert_equal(bi['bip9_softforks']['realloc']['since'], 2500)

            self.log.info("Reward split should stay ~50/50 before the first superblock after activation")
            # This applies even if reallocation was activated right at superblock height like it does here
            bt = self.nodes[0].getblocktemplate()
            assert_equal(bt['height'], 2500)
            assert_equal(bt['masternode'][0]['amount'], get_masternode_payment(bt['height'], bt['coinbasevalue'], 2500))
            self.nodes[0].generate(9)
            bt = self.nodes[0].getblocktemplate()
            assert_equal(bt['masternode'][0]['amount'], get_masternode_payment(bt['height'], bt['coinbasevalue'], 2500))
            assert_equal(bt['coinbasevalue'], 13748571607)
            assert_equal(bt['masternode'][0]['amount'], 6874285801) # 0.4999999998

            self.log.info("Reallocation should kick-in with the superblock mined at height = 2010")
            for _ in range(19):  # there will be 19 adjustments, 3 superblocks long each
                for _ in range(3):
                    self.nodes[0].generate(10)
                    bt = self.nodes[0].getblocktemplate()
                    assert_equal(bt['masternode'][0]['amount'], get_masternode_payment(bt['height'], bt['coinbasevalue'], 2500))

            self.log.info("Reward split should reach ~60/40 after reallocation is done")
            assert_equal(bt['coinbasevalue'], 10221599170)
            assert_equal(bt['masternode'][0]['amount'], 6132959502) # 0.6

            self.log.info("Reward split should stay ~60/40 after reallocation is done")
            for _ in range(10):  # check 10 next superblocks
                self.nodes[0].generate(10)
                bt = self.nodes[0].getblocktemplate()
                assert_equal(bt['masternode'][0]['amount'], get_masternode_payment(bt['height'], bt['coinbasevalue'], 2500))
            assert_equal(bt['coinbasevalue'], 9491484944)
            assert_equal(bt['masternode'][0]['amount'], 5694890966) # 0.6

        if self.options.second_half or (not self.options.first_half and not self.options.second_half):
            self.log.info("Rollback the chain back to the STARTED state")
            self.mocktime = self.nodes[0].getblock(pre_locked_in_blockhash, 1)['time']
            self.nodes[0].invalidateblock(pre_locked_in_blockhash)
            # create and send non-signalling block
            test_block = self.create_test_block()
            self.nodes[0].submitblock(ToHex(test_block))
            bi = self.nodes[0].getblockchaininfo()
            assert_equal(bi['blocks'], 1499)
            assert_equal(bi['bip9_softforks']['realloc']['status'], 'started')
            assert_equal(bi['bip9_softforks']['realloc']['statistics']['threshold'], self.threshold(2))

            self.log.info("Check thresholds reach min level and stay there")
            for i in range(8):  # 7 to reach min level and 1 more to check it doesn't go lower than that
                if i == 4:
                    self.bump_mocktime(1)
                threshold = self.threshold(i + 3)
                self.signal(threshold - 1, False)  # signal just under min threshold as signalling runs faster
                bi = self.nodes[0].getblockchaininfo()
                assert_equal(bi['blocks'], 1999 + i * 500)
                assert_equal(bi['bip9_softforks']['realloc']['status'], 'started')
                assert_equal(bi['bip9_softforks']['realloc']['statistics']['threshold'], threshold)
            assert_equal(bi['bip9_softforks']['realloc']['statistics']['threshold'], 300)


if __name__ == '__main__':
    BlockRewardReallocationTest().main()
