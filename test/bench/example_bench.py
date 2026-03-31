#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from bench_framework import BenchFramework


class ExampleBench(BenchFramework):

    def set_test_params(self) -> None:
        super().set_test_params()

    def run_test(self) -> None:
        super().run_test()

    def set_bench_params(self) -> None:
        self.bench_iterations = 3
        self.bench_name = "example_rpc"
        self.num_nodes = 1
        self.setup_clean_chain = False
        self.warmup_iterations = 1

    def run_bench(self) -> None:
        node = self.nodes[0]

        for _ in range(100):
            self.start_timer()
            node.getblockcount()
            self.stop_timer("getblockcount")

        for _ in range(100):
            self.start_timer()
            node.getbestblockhash()
            self.stop_timer("getbestblockhash")

        for _ in range(50):
            self.start_timer()
            node.getblockchaininfo()
            self.stop_timer("getblockchaininfo")


if __name__ == '__main__':
    ExampleBench().main()
