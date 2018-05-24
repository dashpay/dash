#!/usr/bin/env python3
# Copyright (c) 2015-2018 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test deterministic masternodes
#
from concurrent.futures import ThreadPoolExecutor

from test_framework.blocktools import create_block, create_coinbase, get_masternode_payment
from test_framework.mininode import CTransaction, ToHex, FromHex, CTxOut, COIN, CCbTx
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *

class Masternode(object):
    pass

class QuorumsTest(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_initial_mn = 60
        self.num_nodes = 1 + self.num_initial_mn + 2 # +1 for controller, +1 for mn-qt, +1 for mn created after dip3 activation
        self.setup_clean_chain = True

        self.extra_args = ["-budgetparams=240:100:240"]
        self.extra_args += ["-sporkkey=cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"]
        #self.extra_args += ["-debug=1"]

    def setup_network(self):
        disable_mocktime()
        self.start_controller_node()
        self.is_network_split = False

    def start_controller_node(self, extra_args=None):
        print("starting controller node")
        if self.nodes is None:
            self.nodes = [None]
        args = self.extra_args
        if extra_args is not None:
            args += extra_args
        self.nodes[0] = start_node(0, self.options.tmpdir, extra_args=args)
        for i in range(1, self.num_nodes):
            if i < len(self.nodes) and self.nodes[i] is not None:
                connect_nodes_bi(self.nodes, 0, i)

    def stop_controller_node(self):
        print("stopping controller node")
        stop_node(self.nodes[0], 0)

    def restart_controller_node(self):
        self.stop_controller_node()
        self.start_controller_node()

    def run_test(self):
        print("funding controller node")
        while self.nodes[0].getbalance() < (self.num_initial_mn + 3) * 1000:
            self.nodes[0].generate(10) # generate enough for collaterals
        print("controller node has {} dash".format(self.nodes[0].getbalance()))

        print("generating enough blocks for DIP3 activation")
        while self.nodes[0].getblockchaininfo()['bip9_softforks']['dip0003']['status'] != 'active':
            self.nodes[0].generate(1)
        self.nodes[0].generate(1)
        self.force_finish_mnsync(self.nodes[0])

        print("registering MNs")
        mns = []
        mn_idx = 1
        for i in range(self.num_initial_mn):
            mn = self.create_mn_protx(self.nodes[0], mn_idx, 'mn-%d' % (mn_idx))
            mn_idx += 1
            mns.append(mn)

        # mature collaterals
        for i in range(3):
            self.nodes[0].generate(1)
            time.sleep(1)

        self.sync_all()

        self.stop_controller_node()
        for mn in mns:
            dirs = ['blocks', 'chainstate', 'evodb']
            src = os.path.join(self.options.tmpdir, 'node0', 'regtest')
            dst = os.path.join(mn.datadir, 'regtest')
            for d in dirs:
                shutil.copytree(os.path.join(src, d), os.path.join(dst, d))
        self.start_controller_node()

        print("starting MNs")
        while len(self.nodes) < mn_idx:
            self.nodes.append(None)
        executor = ThreadPoolExecutor(max_workers=20)
        for mn in mns:
            mn.start_job = executor.submit(self.start_mn, mn)
        for mn in mns:
            mn.start_job.result()
            self.force_finish_mnsync(mn.node)

        batchsize = 5
        cnode = 0
        for i in range(0, len(mns), batchsize):
            batch = mns[i:i+batchsize]
            print("connecting %s - %s" % (batch[0].alias, batch[len(batch) - 1].alias))
            for mn in batch:
                connect_nodes(self.nodes[mn.idx], cnode, wait_for_handshake=False)

                random_nodes = []
                for i in range(len(self.nodes)):
                    if i != mn.idx:
                        random_nodes += [i]
                random.shuffle(random_nodes)
                for i in range(min(3, len(random_nodes))):
                    connect_nodes(mn.node, random_nodes[i], wait_for_handshake=False)

            sync_blocks([self.nodes[0]] + [mn.node for mn in batch], timeout=60 * 2)

            # next batch should connect to previous one
            cnode = batch[0].idx

        print("syncing blocks for all nodes")
        sync_blocks(self.nodes, timeout=120)

        # force finishing of mnsync
        for node in self.nodes:
            self.force_finish_mnsync(node)

        print("activating spork15")
        height = self.nodes[0].getblockchaininfo()['blocks']
        spork15_offset = 1
        self.nodes[0].spork('SPORK_15_DETERMINISTIC_MNS_ENABLED', height + spork15_offset)
        self.wait_for_sporks()
        self.nodes[0].generate(1)
        self.sync_all()

        self.assert_mnlists(mns, False, True)

        blsPubKey = '14bc0bc4b8c9ca44bd19cf28d23fcda60ca1179ad11833ab3c46d2837fa77bd38f2d2da44ece213f3898820bc2c62553'
        self.nodes[0].sendtoaddress('yfjAUKch8g9omj5isj9iVr7TZzRrjZk27s', 2000)
        self.nodes[0].importprivkey('cUP3knDebUNdFDRNAivB1Qcw4SQscmQgv7hUTYnuiwk39r3wDieE')
        self.nodes[0].importprivkey('cQfvBFWEA3oMsXo4XV9TLUMgnosNF5MBB2b442P5c28xyoUhY5US')
        # register a MN which you can manually start and debug
        self.nodes[0].protx('register_fund', 'yfjAUKch8g9omj5isj9iVr7TZzRrjZk27s', '127.0.0.1:31001', 'yQbDmr3Ad4bUwu2t4rxkHkcpSovyy8FWfM', blsPubKey, 'yQbDmr3Ad4bUwu2t4rxkHkcpSovyy8FWfM', '0', 'yfjAUKch8g9omj5isj9iVr7TZzRrjZk27s')

        for i in range(10):
            self.nodes[0].generate(10)

        self.sync_all()
        self.nodes[0].spork('SPORK_17_QUORUM_DKG_ENABLED', 0)

        time.sleep(10000)

    def create_mn_protx(self, node, idx, alias):
        mn = Masternode()
        mn.idx = idx
        mn.alias = alias
        mn.is_protx = True
        mn.p2p_port = p2p_port(mn.idx)
        mn.datadir = os.path.join(self.options.tmpdir, "node"+str(idx))

        blsKey = node.bls('generate')
        mn.ownerAddr = node.getnewaddress()
        mn.operatorAddr = blsKey['public']
        mn.votingAddr = mn.ownerAddr
        mn.legacyMnkey = node.masternode('genkey')
        mn.blsMnkey = blsKey['secret']
        mn.collateral_address = node.getnewaddress()

        mn.collateral_txid = node.protx('register_fund', mn.collateral_address, '127.0.0.1:%d' % mn.p2p_port, mn.ownerAddr, mn.operatorAddr, mn.votingAddr, 0, mn.collateral_address)
        rawtx = node.getrawtransaction(mn.collateral_txid, 1)

        mn.collateral_vout = -1
        for txout in rawtx['vout']:
            if txout['value'] == Decimal(1000):
                mn.collateral_vout = txout['n']
                break
        assert(mn.collateral_vout != -1)

        return mn

    def start_mn(self, mn):
        extra_args = ['-masternode=1', '-masternodeprivkey=%s' % mn.legacyMnkey, '-masternodeblsprivkey=%s' % mn.blsMnkey]
        n = start_node(mn.idx, self.options.tmpdir, self.extra_args + extra_args, redirect_stderr=True)
        self.nodes[mn.idx] = n
        mn.node = self.nodes[mn.idx]
        print("started %s" % mn.alias)

    def connect_mns(self, mns):
        for mn in mns:
            connect_nodes(self.nodes[mn.idx], 0)

    def force_finish_mnsync(self, node):
        while True:
            s = node.mnsync('next')
            if s == 'sync updated to MASTERNODE_SYNC_FINISHED':
                break
            #time.sleep(0.01)

    def assert_mnlists(self, mns, include_legacy, include_protx):
        for node in self.nodes:
            self.assert_mnlist(node, mns, include_legacy, include_protx)

    def assert_mnlist(self, node, mns, include_legacy, include_protx):
        if not self.compare_mnlist(node, mns, include_legacy, include_protx):
            expected = []
            for mn in mns:
                if (mn.is_protx and include_protx) or (not mn.is_protx and include_legacy):
                    expected.append('%s-%d' % (mn.collateral_txid, mn.collateral_vout))
            print('mnlist: ' + str(node.masternode('list', 'status')))
            print('expected: ' + str(expected))
            raise AssertionError("mnlists does not match provided mns")

    def wait_for_sporks(self, timeout=30):
        st = time.time()
        while time.time() < st + timeout:
            if self.compare_sporks():
                return
        raise AssertionError("wait_for_sporks timed out")

    def compare_sporks(self):
        sporks = self.nodes[0].spork('show')
        for node in self.nodes[1:]:
            sporks2 = node.spork('show')
            if sporks != sporks2:
                return False
        return True

    def compare_mnlist(self, node, mns, include_legacy, include_protx):
        mnlist = node.masternode('list', 'status')
        for mn in mns:
            s = '%s-%d' % (mn.collateral_txid, mn.collateral_vout)
            in_list = s in mnlist

            if mn.is_protx:
                if include_protx:
                    if not in_list:
                        return False
                else:
                    if in_list:
                        return False
            else:
                if include_legacy:
                    if not in_list:
                        return False
                else:
                    if in_list:
                        return False
            mnlist.pop(s, None)
        if len(mnlist) != 0:
            return False
        return True

if __name__ == '__main__':
    QuorumsTest().main()
