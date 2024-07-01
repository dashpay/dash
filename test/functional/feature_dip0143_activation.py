#!/usr/bin/env python3
# Copyright (c) 2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_dip0143_activation.py
Test correct activation of feature DIP0143
'''
from io import BytesIO

from test_framework.address import key_to_p2pkh
from test_framework.messages import CTransaction, CTxOut, CTxIn, COutPoint, COIN
from test_framework.script import CScript, SIGHASH_ALL, OP_RETURN, DIP0143SignatureHash, SIGHASH_DIP0143
from test_framework.key import ECKey
from test_framework.test_framework import DashTestFramework
from test_framework.util import satoshi_round, hex_str_to_bytes, assert_raises_rpc_error

DISABLED_DIP0143_ERROR = "non-mandatory-script-verify-flag (Attempted to use sigHashType SIGHASH_DIP0143)"


# Generate a random private key to perform ECDSA
def generate_key():
    priv_key = ECKey()
    priv_key.generate()
    pub_key_bytes = priv_key.get_pubkey().get_bytes()
    address = key_to_p2pkh(pub_key_bytes)
    return priv_key, pub_key_bytes, address


class DIP0143ActivationTest(DashTestFramework):

    def set_test_params(self):
        self.set_dash_test_params(4, 3, fast_dip3_enforcement=True)

    def run_test(self):
        self.node = self.nodes[0]
        self.relayfee = satoshi_round(self.nodes[0].getnetworkinfo()["relayfee"])

        # First of all verify that txs signed with DIP0143 algorithm fail before activation
        # Generate a random private key and fund the corresponding address
        priv_key, pub_key_bytes, address = generate_key()
        txid = self.node.sendtoaddress(address, 1)

        # Deserialize and store the result
        txFrom = CTransaction()
        txFrom.deserialize(BytesIO(hex_str_to_bytes(self.node.getrawtransaction(txid))))
        amount = txFrom.vout[0].nValue
        redeem_script = txFrom.vout[0].scriptPubKey

        # Create a new transaction that spends the first output of txFrom
        txTo = CTransaction()
        txTo.vin.append(CTxIn(COutPoint(int(txid, 16), 0), b"", 0xffffffff))
        # We don't really care about the output of txTo, so just set OP_RETURN as redeem script
        txTo.vout.append(CTxOut(int(amount - self.relayfee*COIN), CScript([OP_RETURN])))

        # Serialize with DIP0143 and Sign with ECDSA
        sigHashType = SIGHASH_ALL | SIGHASH_DIP0143
        sigHash = DIP0143SignatureHash(CScript(redeem_script), txTo, 0, sigHashType, amount)
        # Usual P2PKH scriptSig
        txTo.vin[0].scriptSig = CScript([priv_key.sign_ecdsa(sigHash) + bytes(bytearray([sigHashType])), pub_key_bytes])

        # Verify that the transaction is not accepted
        assert_raises_rpc_error(-26, DISABLED_DIP0143_ERROR, self.node.sendrawtransaction, txTo.serialize().hex())
        # TODO: once #5824 is merged activate DIP0143 and test that transaction is accepted


if __name__ == '__main__':
    DIP0143ActivationTest().main()
