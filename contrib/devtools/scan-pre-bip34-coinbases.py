#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Scan pre-BIP34 coinbases for future BIP30-collision targets.

Background
----------
BIP34 ("height in coinbase") does not, by itself, fully imply BIP30
("no duplicate txid").  A pre-BIP34 coinbase had a free-form scriptSig.
If the leading bytes of that scriptSig happen to form a minimal
CScriptNum encoding of some height H > BIP34Height, then at height H a
miner can produce a BIP34-conformant coinbase whose entire transaction
is byte-identical to the historical pre-BIP34 one -- a duplicate txid.

Bitcoin Core handles this with BIP34_IMPLIES_BIP30_LIMIT = 1,983,702,
derived from an exhaustive scan of its own pre-BIP34 coinbases.  Dash
carries no such constant.  Dash mainnet's pre-BIP34 window is heights
1..950; testnet's is 1..75.

This script asks dashd over RPC for each coinbase in the pre-BIP34
range, decodes the leading scriptSig push as a minimal CScriptNum,
and reports every "indicated height" that is greater than the chain's
BIP34Height.  Each such height is a candidate future collision target.

Authentication uses the dashd cookie at <datadir>/<chain>/.cookie.
Pass --rpcuser / --rpcpassword as an explicit override.

Empty output for mainnet implies the BIP30 enforcement machinery in
validation.cpp is dead code on Dash and can be removed.
"""

import argparse
import base64
import json
import os
import sys
import urllib.request


# Per-network defaults: (BIP34Height, default RPC port, on-disk chain folder name).
NETWORK_BIP34 = {
    "main": {"bip34": 951, "port": 9998,  "chain": ""},
    "test": {"bip34": 76,  "port": 19998, "chain": "testnet3"},
}


def read_auth_cookie(datadir, chain_folder):
    """Read <datadir>/<chain_folder>/.cookie -> (user, password)."""
    cookie_path = os.path.join(datadir, chain_folder, ".cookie")
    try:
        with open(cookie_path, "r", encoding="ascii") as f:
            user, password = f.read().split(":", 1)
    except OSError as e:
        raise SystemExit(f"Cannot read cookie at {cookie_path}: {e}.  Pass --rpcuser/--rpcpassword or --datadir.")
    return user, password


def rpc(url, user, password, method, params):
    req = urllib.request.Request(
        url,
        data=json.dumps({"jsonrpc": "1.0", "id": "scan", "method": method, "params": params}).encode(),
        headers={"Content-Type": "application/json"},
    )
    token = base64.b64encode(f"{user}:{password}".encode()).decode()
    req.add_header("Authorization", f"Basic {token}")
    try:
        with urllib.request.urlopen(req) as resp:
            payload = resp.read()
    except urllib.error.HTTPError as e:
        # dashd returns HTTP 500 for JSON-RPC application errors; body still has the error JSON.
        payload = e.read() or b""
        try:
            body = json.loads(payload)
        except ValueError:
            raise RuntimeError(f"{method} {params}: HTTP {e.code} {e.reason} (body: {payload!r})")
        raise RuntimeError(f"{method} {params}: {body.get('error', body)}")
    body = json.loads(payload)
    if body.get("error") is not None:
        raise RuntimeError(f"{method} {params}: {body['error']}")
    return body["result"]


def decode_leading_bip34_height(scriptsig):
    """Interpret the leading bytes of scriptsig as a *minimal* CScriptNum push.

    Returns the indicated height, or None if the prefix is not a valid minimal
    encoding (in which case no future BIP34 block could byte-match this
    coinbase, and there is no collision risk).
    """
    if not scriptsig:
        return None
    b = scriptsig[0]
    # OP_0 -> 0
    if b == 0x00:
        return 0
    # OP_1NEGATE -> -1
    if b == 0x4f:
        return -1
    # OP_1..OP_16 -> 1..16
    if 0x51 <= b <= 0x60:
        return b - 0x50
    # Direct push of 1..75 bytes
    if 1 <= b <= 75:
        n = b
        if len(scriptsig) < 1 + n:
            return None
        payload = scriptsig[1 : 1 + n]
        # Minimality: a 1-byte payload that would fit an OP_N is non-minimal.
        if n == 1 and (payload[0] == 0 or 1 <= payload[0] <= 16 or payload[0] == 0x81):
            return None
        # Minimality: the top byte must carry information (sign bit or magnitude).
        if (payload[-1] & 0x7f) == 0 and (n < 2 or (payload[-2] & 0x80) == 0):
            return None
        # Decode sign-magnitude little-endian
        val = 0
        for i, byte in enumerate(payload):
            val |= byte << (8 * i)
        sign_bit = 0x80 << (8 * (n - 1))
        if val & sign_bit:
            val = -(val & ~sign_bit)
        return val
    # OP_PUSHDATA1/2/4 are not minimal forms for small heights, skip.
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--network", default="main", choices=NETWORK_BIP34.keys())
    p.add_argument("--datadir", default=os.path.expanduser("~/.dashcore"), help="dashd datadir")
    p.add_argument("--rpchost", default="127.0.0.1")
    p.add_argument("--rpcport", type=int, default=None, help="default: per-network (9998/19998/...)")
    p.add_argument("--rpcuser", default=None, help="override; otherwise read from .cookie")
    p.add_argument("--rpcpassword", default=None)
    p.add_argument("--start", type=int, default=1)
    p.add_argument("--end", type=int, default=None, help="default: BIP34Height - 1 for the chosen network")
    p.add_argument("--verbose", action="store_true", help="print every block's decoded height")
    args = p.parse_args()

    net = NETWORK_BIP34[args.network]
    bip34_height = net["bip34"]
    end = args.end if args.end is not None else bip34_height - 1
    if end < args.start:
        print(f"Nothing to scan: BIP34Height={bip34_height}, range [{args.start}, {end}] is empty.")
        return 0

    if args.rpcuser and args.rpcpassword:
        user, password = args.rpcuser, args.rpcpassword
    else:
        user, password = read_auth_cookie(args.datadir, net["chain"])
    port = args.rpcport if args.rpcport is not None else net["port"]
    url = f"http://{args.rpchost}:{port}/"
    print(f"Scanning {args.network} heights {args.start}..{end} (BIP34Height={bip34_height})  @ {url}", file=sys.stderr)

    candidates = []   # (height, indicated_H, coinbase_hex) -- indicated_H > BIP34Height
    unparseable = []  # (height, coinbase_hex) -- decoder rejected as non-minimal
    for h in range(args.start, end + 1):
        block_hash = rpc(url, user, password, "getblockhash", [h])
        block = rpc(url, user, password, "getblock", [block_hash, 2])
        cb_hex = block["tx"][0]["vin"][0]["coinbase"]
        indicated = decode_leading_bip34_height(bytes.fromhex(cb_hex))
        if indicated is None:
            unparseable.append((h, cb_hex))
        elif indicated > bip34_height:
            candidates.append((h, indicated, cb_hex))
        if args.verbose:
            label = "non-minimal" if indicated is None else f"indicated={indicated}"
            print(f"  h={h:5d}  {label:18}  scriptSig={cb_hex}")

    print()
    print(f"Scanned {end - args.start + 1} blocks (heights {args.start}..{end}, BIP34Height={bip34_height}).")
    print(f"  {len(candidates)} candidate future-collision targets (indicated height > BIP34Height).")
    print(f"  {len(unparseable)} scriptSigs not minimally encoding any integer.")
    print(f"    These are safe ONLY IF the decoder above is exhaustive -- please eyeball:")
    for h, cb_hex in unparseable:
        print(f"      h={h:6d}  scriptSig={cb_hex}")

    if candidates:
        print()
        print(f"  {'pre-BIP34 h':>12}  {'indicated H':>12}  coinbase scriptSig (full hex)")
        for h, indicated, cb_hex in candidates:
            print(f"  {h:>12}  {indicated:>12}  {cb_hex}")
        print()
        print("RESULT: Each row above is a candidate future-collision target.  At indicated")
        print("        height H, a miner could in principle re-mine the pre-BIP34 coinbase")
        print("        verbatim (subject to subsidy/fee feasibility) and produce a duplicate")
        print("        txid.  Either keep BIP30 enforcement, audit each H individually, or")
        print("        define a Dash BIP34_IMPLIES_BIP30_LIMIT above the largest indicated H.")
        return 1

    print()
    print("RESULT: No pre-BIP34 coinbase has a leading minimal CScriptNum > BIP34Height.")
    print("        Assuming the decoder is correct (verify against the unparseable list),")
    print("        BIP30 enforcement is unreachable on this chain.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
