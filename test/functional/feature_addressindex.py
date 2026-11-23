#!/usr/bin/env python3
# Copyright (c) 2014-2015 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test addressindex generation and fetching
#

from test_framework.address import address_to_scriptpubkey
from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    COIN,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.script_util import (
    keyhash_to_p2pkh_script,
    scripthash_to_p2sh_script,
)
from test_framework.util import assert_equal, assert_greater_than
from test_framework.wallet import MiniWallet, getnewdestination

class AddressIndexTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.supports_cli = False

    def setup_network(self):
        self.add_nodes(self.num_nodes)
        # Nodes 0/1 are "wallet" nodes
        self.start_node(0, [])
        self.start_node(1, ["-addressindex"])
        # Nodes 2/3 are used for testing
        self.start_node(2, ["-addressindex"])
        self.start_node(3, ["-addressindex"])
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(0, 3)
        self.sync_all()

    def run_test(self):
        self.log.info("Mining blocks...")
        self.wallet = MiniWallet(self.nodes[0])
        mining_address = self.wallet.get_address()
        self.generatetoaddress(self.nodes[0], 105, mining_address)
        self.wallet.rescan_utxos()

        chain_height = self.nodes[1].getblockcount()
        assert_equal(chain_height, 105)

        # Check that balances are correct
        balance0 = self.nodes[1].getaddressbalance("93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB")
        balance_mining = self.nodes[1].getaddressbalance(mining_address)
        assert_equal(balance0["balance"], 0)
        assert_equal(balance_mining["balance"], 105 * 500 * COIN)
        assert_equal(balance_mining["balance_immature"], 100 * 500 * COIN)
        assert_equal(balance_mining["balance_spendable"], 5 * 500 * COIN)

        # Check p2pkh and p2sh address indexes
        self.log.info("Testing p2pkh and p2sh address index...")

        txid0 = self.send_to_address(self.nodes[0], "yMNJePdcKvXtWWQnFYHNeJ5u8TF2v1dfK4", 10)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        txidb0 = self.send_to_address(self.nodes[0], "93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB", 10)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        txid1 = self.send_to_address(self.nodes[0], "yMNJePdcKvXtWWQnFYHNeJ5u8TF2v1dfK4", 15)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        txidb1 = self.send_to_address(self.nodes[0], "93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB", 15)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        txid2 = self.send_to_address(self.nodes[0], "yMNJePdcKvXtWWQnFYHNeJ5u8TF2v1dfK4", 20)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        txidb2 = self.send_to_address(self.nodes[0], "93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB", 20)
        self.generate(self.nodes[0], 1)

        txids = self.nodes[1].getaddresstxids("yMNJePdcKvXtWWQnFYHNeJ5u8TF2v1dfK4")
        assert_equal(len(txids), 3)
        assert_equal(txids[0], txid0)
        assert_equal(txids[1], txid1)
        assert_equal(txids[2], txid2)

        txidsb = self.nodes[1].getaddresstxids("93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB")
        assert_equal(len(txidsb), 3)
        assert_equal(txidsb[0], txidb0)
        assert_equal(txidsb[1], txidb1)
        assert_equal(txidsb[2], txidb2)

        # Check that limiting by height works
        self.log.info("Testing querying txids by range of block heights..")
        height_txids = self.nodes[1].getaddresstxids({
            "addresses": ["93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB"],
            "start": 105,
            "end": 110
        })
        assert_equal(len(height_txids), 2)
        assert_equal(height_txids[0], txidb0)
        assert_equal(height_txids[1], txidb1)

        # Check that multiple addresses works
        multitxids = self.nodes[1].getaddresstxids({"addresses": ["93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB", "yMNJePdcKvXtWWQnFYHNeJ5u8TF2v1dfK4"]})
        assert_equal(len(multitxids), 6)
        assert_equal(multitxids[0], txid0)
        assert_equal(multitxids[1], txidb0)
        assert_equal(multitxids[2], txid1)
        assert_equal(multitxids[3], txidb1)
        assert_equal(multitxids[4], txid2)
        assert_equal(multitxids[5], txidb2)

        # Check that balances are correct
        balance0 = self.nodes[1].getaddressbalance("93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB")
        assert_equal(balance0["balance"], 45 * 100000000)

        # Check that outputs with the same address will only return one txid
        self.log.info("Testing for txid uniqueness...")
        addressHash = bytes.fromhex("FE30B718DCF0BF8A2A686BF1820C073F8B2C3B37")
        scriptPubKey = scripthash_to_p2sh_script(addressHash)
        unspent = [self.wallet.get_utxo()]
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(unspent[0]["txid"], 16), unspent[0]["vout"]))]
        tx.vout = [CTxOut(10 * COIN, scriptPubKey), CTxOut(11 * COIN, scriptPubKey)]
        tx.rehash()

        self.wallet.sign_tx(tx)
        sent_txid = self.nodes[0].sendrawtransaction(tx.serialize().hex(), 0)

        self.generate(self.nodes[0], 1)

        txidsmany = self.nodes[1].getaddresstxids("93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB")
        assert_equal(len(txidsmany), 4)
        assert_equal(txidsmany[3], sent_txid)

        # Check that balances are correct
        self.log.info("Testing balances...")
        balance0 = self.nodes[1].getaddressbalance("93bVhahvUKmQu8gu9g3QnPPa2cxFK98pMB")
        assert_equal(balance0["balance"], (45 + 21) * 100000000)

        # Check that balances are correct after spending
        self.log.info("Testing balances after spending...")
        privkey2 = "cU4zhap7nPJAWeMFu4j6jLrfPmqakDAzy8zn8Fhb3oEevdm4e5Lc"
        address2 = "yeMpGzMj3rhtnz48XsfpB8itPHhHtgxLc3"
        addressHash2 = bytes.fromhex("C5E4FB9171C22409809A3E8047A29C83886E325D")
        scriptPubKey2 = keyhash_to_p2pkh_script(addressHash2)

        unspent = [self.wallet.get_utxo()]
        tx = CTransaction()
        tx_fee_sat = 1000
        tx.vin = [CTxIn(COutPoint(int(unspent[0]["txid"], 16), unspent[0]["vout"]))]
        amount = int(unspent[0]["value"] * 100000000) - tx_fee_sat
        tx.vout = [CTxOut(amount, scriptPubKey2)]
        tx.rehash()
        self.wallet.sign_tx(tx)
        spending_txid = self.nodes[0].sendrawtransaction(tx.serialize().hex(), 0)
        self.generate(self.nodes[0], 1)
        balance1 = self.nodes[1].getaddressbalance(address2)
        assert_equal(balance1["balance"], amount)

        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(spending_txid, 16), 0))]
        send_amount = 1 * 100000000 + 12840
        change_amount = amount - send_amount - 10000
        tx.vout = [CTxOut(change_amount, scriptPubKey2), CTxOut(send_amount, scriptPubKey)]
        tx.rehash()

        signed_tx = self.nodes[0].signrawtransactionwithkey(tx.serialize().hex(), [privkey2])
        sent_txid = self.nodes[0].sendrawtransaction(signed_tx["hex"], 0)
        self.generate(self.nodes[0], 1)

        balance2 = self.nodes[1].getaddressbalance(address2)
        assert_equal(balance2["balance"], change_amount)

        # Check that deltas are returned correctly
        deltas = self.nodes[1].getaddressdeltas({"addresses": [address2], "start": 0, "end": 200})
        balance3 = 0
        for delta in deltas:
            balance3 += delta["satoshis"]
        assert_equal(balance3, change_amount)
        assert_equal(deltas[0]["address"], address2)
        assert_equal(deltas[0]["blockindex"], 1)

        # Check that entire range will be queried
        deltasAll = self.nodes[1].getaddressdeltas({"addresses": [address2]})
        assert_equal(len(deltasAll), len(deltas))
        assert_equal(self.nodes[1].getaddressdeltas(address2), deltasAll)

        # Check that deltas can be returned from range of block heights
        deltas = self.nodes[1].getaddressdeltas({"addresses": [address2], "start": 113, "end": 113})
        assert_equal(len(deltas), 1)

        # Check that unspent outputs can be queried
        self.log.info("Testing utxos...")
        utxos = self.nodes[1].getaddressutxos({"addresses": [address2]})
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]["satoshis"], change_amount)

        # Check that indexes will be updated with a reorg
        self.log.info("Testing reorg...")

        best_hash = self.nodes[0].getbestblockhash()
        self.nodes[0].invalidateblock(best_hash)
        self.nodes[1].invalidateblock(best_hash)
        self.nodes[2].invalidateblock(best_hash)
        self.nodes[3].invalidateblock(best_hash)
        # Allow some time for the reorg to start
        self.bump_mocktime(2)
        self.sync_all()

        balance4 = self.nodes[1].getaddressbalance(address2)
        assert_equal(balance4, balance1)

        utxos2 = self.nodes[1].getaddressutxos({"addresses": [address2]})
        assert_equal(len(utxos2), 1)
        assert_equal(utxos2[0]["satoshis"], amount)

        # Check sorting of utxos
        self.generate(self.nodes[2], 150, sync_fun=self.no_op)

        self.send_to_address(self.nodes[2], address2, 50)
        self.generate(self.nodes[2], 1, sync_fun=self.no_op)
        self.send_to_address(self.nodes[2], address2, 50)
        self.generate(self.nodes[2], 1)

        utxos3 = self.nodes[1].getaddressutxos({"addresses": [address2]})
        assert_equal(len(utxos3), 3)
        assert_equal(utxos3[0]["height"], 114)
        assert_equal(utxos3[1]["height"], 264)
        assert_equal(utxos3[2]["height"], 265)

        # Check mempool indexing
        self.log.info("Testing mempool indexing...")

        privKey3 = "cRyrMvvqi1dmpiCmjmmATqjAwo6Wu7QTjKu1ABMYW5aFG4VXW99K"
        address3 = "yWB15aAdpeKuSaQHFVJpBDPbNSLZJSnDLA"
        addressHash3 = bytes.fromhex("6C186B3A308A77C779A9BB71C3B5A7EC28232A13")
        scriptPubKey3 = keyhash_to_p2pkh_script(addressHash3)
        # address4 = "2N8oFVB2vThAKury4vnLquW2zVjsYjjAkYQ"
        scriptPubKey4 = scripthash_to_p2sh_script(addressHash3)
        unspent = [self.wallet.get_utxo(), self.wallet.get_utxo()]

        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(unspent[0]["txid"], 16), unspent[0]["vout"]))]
        amount = int(unspent[0]["value"] * 100000000) - tx_fee_sat
        tx.vout = [CTxOut(amount, scriptPubKey3)]
        tx.rehash()
        self.wallet.sign_tx(tx)
        memtxid1 = self.nodes[2].sendrawtransaction(tx.serialize().hex(), 0)
        self.bump_mocktime(2)

        tx2 = CTransaction()
        tx2.vin = [CTxIn(COutPoint(int(unspent[1]["txid"], 16), unspent[1]["vout"]))]
        amount = int(unspent[1]["value"] * 100000000) - tx_fee_sat
        tx2.vout = [
            CTxOut(int(amount / 4), scriptPubKey3),
            CTxOut(int(amount / 4), scriptPubKey3),
            CTxOut(int(amount / 4), scriptPubKey4),
            CTxOut(int(amount / 4), scriptPubKey4)
        ]
        tx2.rehash()
        self.wallet.sign_tx(tx2)
        memtxid2 = self.nodes[2].sendrawtransaction(tx2.serialize().hex(), 0)
        self.bump_mocktime(2)

        mempool = self.nodes[2].getaddressmempool({"addresses": [address3]})
        assert_equal(len(mempool), 3)
        assert_equal(mempool[0]["txid"], memtxid1)
        assert_equal(mempool[0]["address"], address3)
        assert_equal(mempool[0]["index"], 0)
        assert_equal(mempool[1]["txid"], memtxid2)
        assert_equal(mempool[1]["index"], 0)
        assert_equal(mempool[2]["txid"], memtxid2)
        assert_equal(mempool[2]["index"], 1)

        self.generate(self.nodes[2], 1)
        mempool2 = self.nodes[2].getaddressmempool({"addresses": [address3]})
        assert_equal(len(mempool2), 0)

        tx = CTransaction()
        tx.vin = [
            CTxIn(COutPoint(int(memtxid2, 16), 0)),
            CTxIn(COutPoint(int(memtxid2, 16), 1))
        ]
        tx.vout = [CTxOut(int(amount / 2 - 10000), scriptPubKey2)]
        tx.rehash()
        signed_tx3 = self.nodes[2].signrawtransactionwithkey(tx.serialize().hex(), [privKey3])
        self.nodes[2].sendrawtransaction(signed_tx3["hex"], 0)
        self.bump_mocktime(2)

        mempool3 = self.nodes[2].getaddressmempool({"addresses": [address3]})
        assert_equal(len(mempool3), 2)
        assert_equal(mempool3[0]["prevtxid"], memtxid2)
        assert_equal(mempool3[0]["prevout"], 0)
        assert_equal(mempool3[1]["prevtxid"], memtxid2)
        assert_equal(mempool3[1]["prevout"], 1)

        # sending and receiving to the same address
        privkey1 = "cMvZn1pVWntTEcsK36ZteGQXRAcZ8CoTbMXF1QasxBLdnTwyVQCc"
        address1 = "yM9Eed1bxjy7tYxD3yZDHxjcVT48WdRoB1"
        address1hash = bytes.fromhex("0909C84A817651502E020AAD0FBCAE5F656E7D8A")
        address1script = keyhash_to_p2pkh_script(address1hash)

        self.send_to_address(self.nodes[0], address1, 10)
        self.generate(self.nodes[0], 1)

        utxos = self.nodes[1].getaddressutxos({"addresses": [address1]})
        assert_equal(len(utxos), 1)

        tx = CTransaction()
        tx.vin = [
            CTxIn(COutPoint(int(utxos[0]["txid"], 16), utxos[0]["outputIndex"]))
        ]
        amount = int(utxos[0]["satoshis"] - 10000)
        tx.vout = [CTxOut(amount, address1script)]
        tx.rehash()
        signed_tx = self.nodes[0].signrawtransactionwithkey(tx.serialize().hex(), [privkey1])
        self.nodes[0].sendrawtransaction(signed_tx["hex"], 0)

        self.sync_all()
        mempool_deltas = self.nodes[2].getaddressmempool({"addresses": [address1]})
        assert_equal(len(mempool_deltas), 2)

        self.test_reorg_while_disabled()

        self.log.info("Passed")

    def send_to_address(self, node, address, amount):
        return self.wallet.send_to(from_node=node, scriptPubKey=address_to_scriptpubkey(address), amount=amount * COIN)["txid"]

    def wait_index_synced(self, node):
        self.wait_until(lambda: node.getindexinfo()["addressindex"] == {"synced": True, "best_block_height": node.getblockcount()})

    def test_reorg_while_disabled(self):
        self.log.info("Test that blocks orphaned while the index was disabled are removed from it")
        node = self.nodes[3]
        self.disconnect_nodes(0, 3)
        orphan_address = getnewdestination()[2]
        orphan_block = self.generatetoaddress(node, 1, orphan_address, sync_fun=self.no_op)[0]
        self.wait_index_synced(node)
        assert_greater_than(node.getaddressbalance(orphan_address)["received"], 0)

        self.restart_node(3, ["-addressindex=0"])
        node.invalidateblock(orphan_block)
        new_address = getnewdestination()[2]
        block_hashes = self.generatetoaddress(node, 2, new_address, sync_fun=self.no_op)
        # Greater-than-zero would pass if only one of the two blocks was indexed.
        paid = sum(vout["valueSat"]
                   for block_hash in block_hashes
                   for vout in node.getblock(block_hash, 2)["tx"][0]["vout"]
                   if vout["scriptPubKey"].get("address") == new_address)

        self.restart_node(3, ["-addressindex"])
        self.wait_index_synced(node)
        assert_equal(node.getaddressbalance(new_address)["received"], paid)
        assert_equal(node.getaddressbalance(orphan_address)["received"], 0)
        assert_equal(node.getaddresstxids(orphan_address), [])


if __name__ == '__main__':
    AddressIndexTest().main()
