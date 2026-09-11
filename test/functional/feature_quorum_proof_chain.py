#!/usr/bin/env python3
# Copyright (c) 2025-2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Verify real testnet mining proofs via RPC with an independent checkpoint."""
import json
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class QuorumProofChainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.supports_cli = False  # Exercises HTTP batches and explicit CLI calls.

    def run_test(self):
        node = self.nodes[0]
        fixture = json.loads((Path(__file__).parent / "data/quorum_proof.json").read_text())
        anchor, proof = fixture["checkpoint"], fixture["proof_hex"]
        result = node.verifyquorumproofchain(anchor, proof)
        assert_equal(result, {"valid": True, "target": fixture["target"]})
        assert_raises_rpc_error(-8, "height must be non-negative", node.cli.getchainlockbyheight, -1)
        assert_raises_rpc_error(-8, "height must be non-negative", node.cli.getchainlockbyheight, height=-1)
        assert_raises_rpc_error(-1, "No archived certificate", node.cli.getquorumproofchain,
                                checkpoint_hash=node.getbestblockhash(), height=0, llmq_type=0, node_count=4)
        assert_equal(node.cli.verifyquorumproofchain(checkpoint=anchor, proof_hex=proof, minimum_height=0), result)

        # An unavailable archive request must not prevent independent verification
        # in the same batch. This fixture has no archived ChainLock.
        requests = [
            node.getquorumproofchain.get_request(checkpoint_hash=node.getbestblockhash()),
            node.getchainlockbyheight.get_request(height=0),
            node.verifyquorumproofchain.get_request(checkpoint=anchor, proof_hex=proof),
        ]
        responses = {response["id"]: response for response in node.batch(requests)}
        assert_equal(responses[requests[0]["id"]]["error"]["code"], -1)
        assert "No archived certificate" in responses[requests[0]["id"]]["error"]["message"]
        assert_equal(responses[requests[1]["id"]]["error"],
                     {"code": -5, "message": "Chainlock not found for height"})
        assert_equal(responses[requests[2]["id"]]["error"], None)
        assert_equal(responses[requests[2]["id"]]["result"], result)
        assert_equal(node.verifyquorumproofchain(anchor, proof, fixture["target"]["height"] + 1)["valid"], False)
        wrong = dict(anchor, quorum_root="01" * 32)
        assert_equal(node.verifyquorumproofchain(wrong, proof)["valid"], False)
        for invalid in (proof[:-2], proof + "00", "444153484e433031" + proof[16:]):
            assert_equal(node.verifyquorumproofchain(anchor, invalid)["valid"], False)
        assert_raises_rpc_error(-8, "Proof hex size/encoding", node.verifyquorumproofchain, anchor, "00" * 1048577)
        assert_raises_rpc_error(-8, "snapshot fields missing", node.verifyquorumproofchain,
                                {"height": anchor["height"]}, proof)
        assert_raises_rpc_error(-8, None, node.verifyquorumproofchain, anchor, proof, -1)
        tampered = bytearray.fromhex(proof)
        tampered[-20] ^= 1
        assert_equal(node.verifyquorumproofchain(anchor, tampered.hex())["valid"], False)
        assert_raises_rpc_error(-1, "No archived certificate", node.getquorumproofchain, node.getbestblockhash())
        assert_raises_rpc_error(-8, "Invalid proof request", node.getquorumproofchain, node.getbestblockhash(), 0, "", 0, 16)
        self.generatetoaddress(node, 440, node.get_deterministic_priv_key().address)
        self.restart_node(0)
        assert_equal(node.verifyquorumproofchain(anchor, proof)["valid"], True)
        assert_raises_rpc_error(-1, "No archived certificate", node.getquorumproofchain, node.getbestblockhash())
        self.restart_node(0)
        assert_equal(node.verifyquorumproofchain(anchor, proof)["valid"], True)


if __name__ == "__main__":
    QuorumProofChainTest().main()
