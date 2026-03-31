#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import time
from typing import Any, List

from bench_framework import BenchFramework
from bench_helpers import create_self_transfer_batch, zmq_subscribe, zmq_receive_one

from test_framework.authproxy import JSONRPCException  # noqa: E402
from test_framework.util import p2p_port  # noqa: E402
from test_framework.wallet import MiniWallet  # noqa: E402

# Test may be skipped and not have zmq installed
try:
    import zmq
except ImportError:
    pass


class ZmqBench(BenchFramework):

    def set_test_params(self) -> None:
        super().set_test_params()

    def run_test(self) -> None:
        super().run_test()

    def add_options(self, parser) -> None:  # type: ignore[override]
        super().add_options(parser)
        parser.add_argument(
            "--tx-count",
            dest="tx_count",
            type=int,
            default=50,
            help="Transactions to send (default: 50)",
        )

    def skip_test_if_missing_module(self) -> None:
        self.skip_if_no_bitcoind_zmq()
        self.skip_if_no_py3_zmq()

    def set_bench_params(self) -> None:
        self._tx_count = 50
        self._zmq_port = 0
        self.bench_iterations = 1
        self.bench_name = "zmq_notification_latency"
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.warmup_iterations = 0

    def setup_network(self) -> None:
        self._zmq_port = p2p_port(self.num_nodes + 10)
        zmq_addr = f"tcp://127.0.0.1:{self._zmq_port}"
        self.extra_args = [[
            f"-zmqpubhashtx={zmq_addr}",
        ]]
        self.setup_nodes()

    def run_bench(self) -> None:
        self._tx_count = self.options.tx_count
        node = self.nodes[0]
        wallet = MiniWallet(node)
        zmq_addr = f"tcp://127.0.0.1:{self._zmq_port}"

        num_blocks = self._tx_count + 110
        self.log.info("Mining %d blocks for funding...", num_blocks)
        self.generate(wallet, num_blocks, sync_fun=self.no_op)

        # Pre-create transactions
        self.log.info("Creating %d transactions...", self._tx_count)
        tx_hexes: List[str] = create_self_transfer_batch(wallet, self._tx_count)

        # Subscribe to hashtx notifications
        ctx: Any
        sock: Any
        ctx, sock = zmq_subscribe(zmq_addr, b"hashtx")

        try:
            # Generate a block and consume its notification so the subscriber is fully connected before timing
            self.generate(node, 1, sync_fun=self.no_op)
            try:
                while True:
                    sock.recv_multipart(flags=zmq.NOBLOCK)
            except zmq.Again:
                pass

            # Send transactions and measure notification latency
            self.log.info("Submitting %d transactions...", len(tx_hexes))
            received = 0
            for tx_hex in tx_hexes:
                t_send = time.perf_counter()
                try:
                    node.sendrawtransaction(tx_hex)
                except JSONRPCException:
                    continue
                try:
                    _topic, _body, t_recv = zmq_receive_one(sock)
                    latency_ms = (t_recv - t_send) * 1000.0
                    self.record_sample("zmq_hashtx_latency", latency_ms)
                    received += 1
                except zmq.Again:
                    self.log.warning("ZMQ timeout waiting for notification")

            self.log.info(
                "Received %d/%d ZMQ notifications",
                received, len(tx_hexes),
            )
        finally:
            sock.close()
            ctx.destroy(linger=0)


if __name__ == '__main__':
    ZmqBench().main()
