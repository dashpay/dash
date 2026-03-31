#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Test REST server command line options and argument validation."""

import os

from test_framework.test_framework import BitcoinTestFramework

# See rest::Options in rest/server.h
DEFAULT_ADDR = '127.0.0.1'
DEFAULT_PORT = 19897  # regtest
DEFAULT_THREADS = 4
DEFAULT_MAX_CONNECTIONS = 100
DEFAULT_IDLE_TIMEOUT = 30
DEFAULT_REUSE_PORT = 0

COMMON_ARGS = ['-rest', '-debug=rest']


def starting_server_msg(addr=DEFAULT_ADDR, port=DEFAULT_PORT, threads=DEFAULT_THREADS,
                        max_conn=DEFAULT_MAX_CONNECTIONS, idle_timeout=DEFAULT_IDLE_TIMEOUT,
                        reuseport=DEFAULT_REUSE_PORT):
    return (f'Starting server on {addr}:{port} with {threads} thread(s), '
            f'max_conn={max_conn}, idle_timeout={idle_timeout}, reuseport={reuseport}')


class RESTFeatureTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_drogon()

    def run_test(self):
        self.log.info("Test invalid command line options")
        self.test_invalid_port()
        self.test_invalid_bind()
        self.test_invalid_idle_timeout()
        self.test_invalid_max_connections()
        self.test_invalid_threads()

        self.log.info("Test command line behavior")
        self.test_clamping_warnings()
        self.test_defaults()

    def test_invalid_port(self):
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(
            expected_msg='Error: Invalid -restport value: 0 (must be between 1 and 65535)',
            extra_args=['-rest', '-restport=0'],
        )
        self.nodes[0].assert_start_raises_init_error(
            expected_msg='Error: Invalid -restport value: -1 (must be between 1 and 65535)',
            extra_args=['-rest', '-restport=-1'],
        )
        self.nodes[0].assert_start_raises_init_error(
            expected_msg='Error: Invalid -restport value: 65536 (must be between 1 and 65535)',
            extra_args=['-rest', '-restport=65536'],
        )

    def test_invalid_bind(self):
        self.nodes[0].assert_start_raises_init_error(
            expected_msg="Error: Cannot resolve -restbind address: 'not_a_valid_addr'",
            extra_args=['-rest', '-restbind=not_a_valid_addr'],
        )

    def test_invalid_idle_timeout(self):
        warning_str = 'Warning: -restidletimeout below 5 seconds or above 3600 seconds, clamped to %d seconds'
        with self.nodes[0].assert_debug_log(expected_msgs=[
            warning_str % 5,
            starting_server_msg(idle_timeout=5),
        ]):
            self.start_node(0, extra_args=COMMON_ARGS + ['-restidletimeout=1'])
        self.stop_node(0, expected_stderr=warning_str % 5)
        with self.nodes[0].assert_debug_log(expected_msgs=[
            warning_str % 3600,
            starting_server_msg(idle_timeout=3600),
        ]):
            self.start_node(0, extra_args=COMMON_ARGS + ['-restidletimeout=7200'])
        self.stop_node(0, expected_stderr=warning_str % 3600)

    def test_invalid_max_connections(self):
        warning_str = 'Warning: -restmaxconnections below 1 connections or above 65535 connections, clamped to 1 connections'
        with self.nodes[0].assert_debug_log(expected_msgs=[
            warning_str,
            starting_server_msg(max_conn=1),
        ]):
            self.start_node(0, extra_args=COMMON_ARGS + ['-restmaxconnections=0'])
        self.stop_node(0, expected_stderr=warning_str)
        with self.nodes[0].assert_debug_log(expected_msgs=[
            warning_str,
            starting_server_msg(max_conn=1),
        ]):
            self.start_node(0, extra_args=COMMON_ARGS + ['-restmaxconnections=-5'])
        self.stop_node(0, expected_stderr=warning_str)

    def test_invalid_threads(self):
        num_cores = os.cpu_count()
        warning_str = 'Warning: -restthreads below 1 thread or above %d threads, clamped to %d threads.'
        warning_low = warning_str % (num_cores, 1)
        warning_high = warning_str % (num_cores, num_cores)
        with self.nodes[0].assert_debug_log(expected_msgs=[
            warning_low,
            starting_server_msg(threads=1),
        ]):
            self.start_node(0, extra_args=COMMON_ARGS + ['-restthreads=0'])
        self.stop_node(0, expected_stderr=warning_low)
        with self.nodes[0].assert_debug_log(expected_msgs=[
            warning_low,
            starting_server_msg(threads=1),
        ]):
            self.start_node(0, extra_args=COMMON_ARGS + ['-restthreads=-1'])
        self.stop_node(0, expected_stderr=warning_low)
        with self.nodes[0].assert_debug_log(expected_msgs=[
            warning_high,
            starting_server_msg(threads=num_cores),
        ]):
            self.start_node(0, extra_args=COMMON_ARGS + ['-restthreads=99999'])
        self.stop_node(0, expected_stderr=warning_high)

    def test_clamping_warnings(self):
        with self.nodes[0].assert_debug_log(
            expected_msgs=[starting_server_msg(threads=1, max_conn=50, idle_timeout=30)],
            unexpected_msgs=[
                'clamped to',
            ],
        ):
            self.start_node(0, extra_args=COMMON_ARGS + ['-restidletimeout=30', '-restmaxconnections=50', '-restthreads=1'])
        self.stop_node(0)

    def test_defaults(self):
        with self.nodes[0].assert_debug_log(expected_msgs=[
            starting_server_msg(),
        ]):
            self.start_node(0, extra_args=COMMON_ARGS)
        self.stop_node(0)


if __name__ == '__main__':
    RESTFeatureTest().main()
