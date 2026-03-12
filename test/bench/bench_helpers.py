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


async def async_rest_flood(
    host: str,
    port: int,
    path: str,
    concurrency: int = 50,
    duration_s: float = 10.0,
    force_close: bool = False,
    connect_burst: int = 0,
) -> Dict[str, Any]:
    """Flood a REST endpoint with concurrent HTTP GET requests."""
    url = f"http://{host}:{port}{path}"
    latencies: List[float] = []
    success = 0
    failed = 0
    status_codes: Dict[str, int] = {}
    bytes_rx = 0
    deadline = time.monotonic() + duration_s

    # Share a single connector across all workers so that connection
    # establishment is serialised through aiohttp's pool rather than
    # each worker racing to open its own TCP socket.  This avoids
    # overwhelming the server's accept backlog (SOMAXCONN=128 on macOS).
    kwargs: Dict[str, Any] = {"limit": 0, "force_close": force_close}
    if not force_close:
        kwargs["keepalive_timeout"] = 60
    shared_conn = aiohttp.TCPConnector(**kwargs)

    # Semaphore that gates only the *first* request from each worker (the one
    # that opens the TCP connection).  After the connection is in the keep-alive
    # pool, subsequent requests reuse it and never block on the semaphore.
    connect_sem: Optional[asyncio.Semaphore] = (
        asyncio.Semaphore(connect_burst)
        if (connect_burst > 0 and not force_close)
        else None
    )

    async def worker(session: aiohttp.ClientSession) -> None:
        nonlocal success, failed, bytes_rx
        needs_connect = connect_sem is not None
        while time.monotonic() < deadline:
            t0 = time.perf_counter()
            try:
                if needs_connect:
                    assert connect_sem is not None
                    async with connect_sem:
                        async with session.get(
                            url,
                            timeout=aiohttp.ClientTimeout(total=30),
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
                    needs_connect = False
                else:
                    async with session.get(
                        url,
                        timeout=aiohttp.ClientTimeout(total=30),
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

    async with aiohttp.ClientSession(connector=shared_conn) as session:
        tasks = [asyncio.create_task(worker(session)) for _ in range(concurrency)]
        await asyncio.gather(*tasks)

    return {
        "latencies_ms": latencies,
        "success": success,
        "failed": failed,
        "status_codes": status_codes,
        "bytes_received": bytes_rx,
    }


async def async_rest_discover(
    host: str,
    port: int,
) -> Tuple[Optional[str], Optional[str]]:
    """Probe a REST server for a light and heavy endpoint."""
    light_path: Optional[str] = None
    heavy_path: Optional[str] = None
    best_hash: Optional[str] = None

    conn = aiohttp.TCPConnector(force_close=True)
    async with aiohttp.ClientSession(connector=conn) as session:
        for path in ["/rest/chaininfo.json", "/rest/mempool/info.json"]:
            try:
                async with session.get(
                    f"http://{host}:{port}{path}",
                    timeout=aiohttp.ClientTimeout(total=5),
                ) as resp:
                    if resp.status == 200:
                        body = await resp.json(content_type=None)
                        light_path = path
                        if "bestblockhash" in body:
                            best_hash = body["bestblockhash"]
                        break
            except Exception:
                continue

        if best_hash:
            heavy_candidate = f"/rest/block/{best_hash}.json"
            try:
                async with session.get(
                    f"http://{host}:{port}{heavy_candidate}",
                    timeout=aiohttp.ClientTimeout(total=15),
                ) as resp:
                    if resp.status == 200:
                        heavy_path = heavy_candidate
            except Exception:
                pass

    return light_path, heavy_path


def zmq_subscribe(
    address: str,
    topic: bytes,
    timeout_ms: int = 30000,
) -> Tuple[Any, Any]:
    """Create a ZMQ SUB socket connected to *address* with *topic*.
    Caller must remember to ``socket.close()`` and ``context.destroy()`` when done.
    """
    import zmq
    ctx = zmq.Context()
    sock = ctx.socket(zmq.SUB)
    sock.set(zmq.RCVTIMEO, timeout_ms)
    sock.set(zmq.IPV6, 1)
    sock.setsockopt(zmq.SUBSCRIBE, topic)
    sock.connect(address)
    return ctx, sock


def zmq_receive_one(
    sock: Any,
) -> Tuple[bytes, bytes, float]:
    """Receive one ZMQ multipart message from *sock*."""
    topic, body, _seq = sock.recv_multipart()
    return topic, body, time.perf_counter()
