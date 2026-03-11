#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import asyncio
import json
import os
import sys
import time
import urllib.parse
from typing import Any, Dict, List, Optional, Tuple

import aiohttp

# Allow imports from the functional test framework.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'functional'))

from test_framework.authproxy import JSONRPCException  # noqa: E402
from test_framework.test_node import TestNode  # noqa: E402
from test_framework.wallet import MiniWallet  # noqa: E402


def create_self_transfer_batch(
    wallet: MiniWallet,
    count: int,
) -> List[str]:
    """Create *count* signed self-transfer transactions from *wallet* UTXOs."""
    txs: List[str] = []
    for _ in range(count):
        tx = wallet.create_self_transfer()
        txs.append(tx["hex"])
    return txs


def submit_transactions(
    node: TestNode,
    tx_hexes: List[str],
) -> List[str]:
    """Submit a list of raw transactions to *node* via ``sendrawtransaction``."""
    txids: List[str] = []
    for tx_hex in tx_hexes:
        try:
            txid = node.sendrawtransaction(tx_hex)
            txids.append(txid)
        except JSONRPCException:
            pass
    return txids


def _parse_rpc_url(node: TestNode) -> Tuple[str, Optional[aiohttp.BasicAuth]]:
    """Extract base URL and auth from a TestNode's RPC URL."""
    parsed = urllib.parse.urlparse(node.url)
    auth: Optional[aiohttp.BasicAuth] = None
    if parsed.username:
        auth = aiohttp.BasicAuth(parsed.username, parsed.password or "")
    base = f"{parsed.scheme}://{parsed.hostname}:{parsed.port}"
    return base, auth


def _rpc_payload(method: str, params: Optional[List[Any]] = None) -> bytes:
    """Build a RPC request body."""
    return json.dumps({
        "version": "1.1",
        "id": 1,
        "method": method,
        "params": params or [],
    }).encode()


async def async_rpc_flood(
    node: TestNode,
    method: str,
    params: Optional[List[Any]] = None,
    concurrency: int = 50,
    duration_s: float = 10.0,
) -> Dict[str, Any]:
    """Flood a node's RPC endpoint with concurrent requests."""
    base_url, auth = _parse_rpc_url(node)
    latencies: List[float] = []
    success = 0
    failed = 0
    status_codes: Dict[str, int] = {}
    bytes_rx = 0
    deadline = time.monotonic() + duration_s

    async def worker() -> None:
        nonlocal success, failed, bytes_rx
        conn = aiohttp.TCPConnector(limit=0, keepalive_timeout=60)
        async with aiohttp.ClientSession(connector=conn) as session:
            while time.monotonic() < deadline:
                payload = _rpc_payload(method, params)
                t0 = time.perf_counter()
                try:
                    async with session.post(
                        base_url,
                        data=payload,
                        auth=auth,
                        headers={"Content-Type": "application/json"},
                        timeout=aiohttp.ClientTimeout(total=10),
                    ) as resp:
                        body = await resp.read()
                        elapsed_ms = (time.perf_counter() - t0) * 1000.0
                        latencies.append(elapsed_ms)
                        bytes_rx += len(body)
                        key = str(resp.status)
                        status_codes[key] = status_codes.get(key, 0) + 1
                        if resp.status == 200:
                            success += 1
                        else:
                            failed += 1
                except Exception as e:
                    elapsed_ms = (time.perf_counter() - t0) * 1000.0
                    latencies.append(elapsed_ms)
                    failed += 1
                    key = type(e).__name__
                    status_codes[key] = status_codes.get(key, 0) + 1

    tasks = [asyncio.create_task(worker()) for _ in range(concurrency)]
    await asyncio.gather(*tasks)

    return {
        "latencies_ms": latencies,
        "success": success,
        "failed": failed,
        "status_codes": status_codes,
        "bytes_received": bytes_rx,
    }
