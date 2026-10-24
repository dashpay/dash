#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test sigop limit mempool policy (`-bytespersigop` parameter)"""
from math import ceil

from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_CHECKMULTISIG,
    OP_CHECKSIG,
    OP_ENDIF,
    OP_FALSE,
    OP_IF,
    OP_RETURN,
    OP_TRUE,
)
from test_framework.script_util import (
    script_to_p2sh_script,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
)
from test_framework.wallet import MiniWallet


DEFAULT_BYTES_PER_SIGOP = 20  # default setting
MAX_STANDARD_TX_SIZE = 100000  # Dash consensus tx size limit (after DIP0001)


class BytesPerSigOpTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # allow large datacarrier output to pad transactions
        # Dash has no segwit, so P2SH is used instead of P2WSH; allow non-standard
        # txs to bypass the MAX_P2SH_SIGOPS (15) standardness limit
        self.extra_args = [['-datacarriersize=100000', '-acceptnonstdtxn=1']]

    def create_p2sh_spending_tx(self, redeem_script, output_script):
        """Create a 1-input-1-output P2SH spending transaction with only the
           redeem script in the scriptSig and the given output script."""
        # create P2SH address and fund it via MiniWallet first
        res = self.wallet.send_to(
            from_node=self.nodes[0],
            scriptPubKey=script_to_p2sh_script(redeem_script),
            amount=1000000,
        )

        # create spending transaction
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(res["txid"], 16), res["sent_vout"]), CScript([bytes(redeem_script)]))]
        tx.vout = [CTxOut(500000, output_script)]
        return tx

    def test_sigops_limit(self, bytes_per_sigop, num_sigops):
        sigop_equivalent_vsize = ceil(num_sigops * bytes_per_sigop)
        self.log.info(f"- {num_sigops} sigops (equivalent size of {sigop_equivalent_vsize} vbytes)")

        # create a template tx with the specified sigop cost in the redeem script
        # (note that the sigops count even though being in a branch that's not executed)
        num_multisigops = num_sigops // 20
        num_singlesigops = num_sigops % 20
        redeem_script = CScript(
            [OP_FALSE, OP_IF] +
            [OP_CHECKMULTISIG]*num_multisigops +
            [OP_CHECKSIG]*num_singlesigops +
            [OP_ENDIF, OP_TRUE]
        )
        # use a 256-byte data-push as lower bound in the output script, in order
        # to avoid having to compensate for tx size changes caused by varying
        # length serialization sizes (both for scriptPubKey and data-push lengths)
        tx = self.create_p2sh_spending_tx(redeem_script, CScript([OP_RETURN, b'X'*256]))

        # bump the tx to reach the sigop-limit equivalent size by padding the datacarrier output
        assert_greater_than_or_equal(sigop_equivalent_vsize, tx.get_vsize())
        vsize_to_pad = sigop_equivalent_vsize - tx.get_vsize()
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'X'*(256+vsize_to_pad)])
        # without witness discount (Dash has no segwit) the padding can exceed
        # 65535 bytes, growing the push/script length serialization; compensate
        vsize_to_pad -= tx.get_vsize() - sigop_equivalent_vsize
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'X'*(256+vsize_to_pad)])
        assert_equal(sigop_equivalent_vsize, tx.get_vsize())

        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res['allowed'], True)
        assert_equal(res['vsize'], sigop_equivalent_vsize)

        # increase the tx's vsize to be right above the sigop-limit equivalent size
        # => tx's vsize in mempool should also grow accordingly
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'X'*(256+vsize_to_pad+1)])
        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res['allowed'], True)
        assert_equal(res['vsize'], sigop_equivalent_vsize+1)

        # decrease the tx's vsize to be right below the sigop-limit equivalent size
        # => tx's vsize in mempool should stick at the sigop-limit equivalent
        # bytes level, as it is higher than the tx's serialized vsize
        # (the maximum of both is taken)
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'X'*(256+vsize_to_pad-1)])
        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res['allowed'], True)
        assert_equal(res['vsize'], sigop_equivalent_vsize)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])

        for bytes_per_sigop in (DEFAULT_BYTES_PER_SIGOP, 43, 81, 165, 327, 649, 1072):
            if bytes_per_sigop == DEFAULT_BYTES_PER_SIGOP:
                self.log.info(f"Test default sigops limit setting ({bytes_per_sigop} bytes per sigop)...")
            else:
                bytespersigop_parameter = f"-bytespersigop={bytes_per_sigop}"
                self.log.info(f"Test sigops limit setting {bytespersigop_parameter}...")
                self.restart_node(0, extra_args=[bytespersigop_parameter] + self.extra_args[0])

            for num_sigops in (69, 101, 142, 183, 222):
                # without the witness discount, larger combinations exceed Dash's tx size limit
                if num_sigops * bytes_per_sigop >= MAX_STANDARD_TX_SIZE:
                    continue
                self.test_sigops_limit(bytes_per_sigop, num_sigops)


if __name__ == '__main__':
    BytesPerSigOpTest().main()
