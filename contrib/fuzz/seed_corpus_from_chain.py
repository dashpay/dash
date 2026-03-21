#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Extract seed corpus inputs from a running Dash node for fuzz testing.

Connects to a local dashd via RPC and extracts real-world serialized data
(transactions, blocks, special transactions, governance objects, etc.)
into fuzzer-consumable corpus files.

Usage:
    ./seed_corpus_from_chain.py --output-dir /path/to/corpus [options]

Requirements:
    - Running dashd with RPC enabled
    - python-bitcoinrpc or compatible RPC library (or uses subprocess + dash-cli)
"""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def dash_cli(*args, datadir=None):
    """Call dash-cli and return the result."""
    cmd = ["dash-cli"]
    if datadir:
        cmd.append(f"-datadir={datadir}")
    cmd.extend(args)
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=30)
        return result.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as e:
        print(f"WARNING: dash-cli {' '.join(args)} failed: {e}", file=sys.stderr)
        return None


def save_corpus_input(output_dir, target_name, data_hex):
    """Save a hex-encoded blob as a corpus input file."""
    target_dir = output_dir / target_name
    target_dir.mkdir(parents=True, exist_ok=True)

    try:
        raw_bytes = bytes.fromhex(data_hex)
    except ValueError:
        print(f"WARNING: Invalid hex data for target {target_name}, skipping", file=sys.stderr)
        return False

    filename = hashlib.sha256(raw_bytes).hexdigest()[:16]
    filepath = target_dir / filename

    if not filepath.exists():
        filepath.write_bytes(raw_bytes)
        return True
    return False


def read_compact_size(raw, offset):
    """Decode a CompactSize integer from raw bytes at offset."""
    if offset >= len(raw):
        raise ValueError("truncated CompactSize")

    first = raw[offset]
    offset += 1
    if first < 253:
        return first, offset
    if first == 253:
        if offset + 2 > len(raw):
            raise ValueError("truncated CompactSize (uint16)")
        return int.from_bytes(raw[offset:offset + 2], byteorder="little"), offset + 2
    if first == 254:
        if offset + 4 > len(raw):
            raise ValueError("truncated CompactSize (uint32)")
        return int.from_bytes(raw[offset:offset + 4], byteorder="little"), offset + 4
    if offset + 8 > len(raw):
        raise ValueError("truncated CompactSize (uint64)")
    return int.from_bytes(raw[offset:offset + 8], byteorder="little"), offset + 8


def extract_extra_payload_hex(raw_tx_hex, extra_payload_size):
    """Extract extra payload bytes by parsing a raw special transaction."""
    try:
        raw_tx = bytes.fromhex(raw_tx_hex)
    except ValueError:
        return None, "raw transaction is not valid hex"

    if extra_payload_size <= 0:
        return None, "extraPayloadSize must be > 0"

    try:
        offset = 0
        if len(raw_tx) < 4:
            return None, "raw transaction too short for nVersion/nType"

        n32bit_version = int.from_bytes(raw_tx[offset:offset + 4], byteorder="little")
        n_version = n32bit_version & 0xFFFF
        n_type = (n32bit_version >> 16) & 0xFFFF
        offset += 4

        if n_version < 3 or n_type == 0:
            return None, f"transaction is not a special tx (version={n_version}, type={n_type})"

        vin_count, offset = read_compact_size(raw_tx, offset)
        for _ in range(vin_count):
            # CTxIn: prevout hash (32), prevout index (4), scriptSig, sequence (4)
            if offset + 36 > len(raw_tx):
                return None, "truncated tx input prevout"
            offset += 36

            script_len, offset = read_compact_size(raw_tx, offset)
            if offset + script_len + 4 > len(raw_tx):
                return None, "truncated tx input scriptSig/sequence"
            offset += script_len + 4

        vout_count, offset = read_compact_size(raw_tx, offset)
        for _ in range(vout_count):
            # CTxOut: amount (8), scriptPubKey
            if offset + 8 > len(raw_tx):
                return None, "truncated tx output amount"
            offset += 8

            script_len, offset = read_compact_size(raw_tx, offset)
            if offset + script_len > len(raw_tx):
                return None, "truncated tx output scriptPubKey"
            offset += script_len

        if offset + 4 > len(raw_tx):
            return None, "truncated nLockTime"
        offset += 4

        payload_len, offset = read_compact_size(raw_tx, offset)
        if payload_len != extra_payload_size:
            return None, f"extra payload size mismatch (expected {extra_payload_size}, parsed {payload_len})"
        if offset + payload_len > len(raw_tx):
            return None, "truncated extra payload"

        payload = raw_tx[offset:offset + payload_len]
        offset += payload_len
        if offset != len(raw_tx):
            return None, f"unexpected trailing bytes after payload ({len(raw_tx) - offset} bytes)"

        return payload.hex(), None
    except ValueError as e:
        return None, str(e)


def extract_blocks(output_dir, count=20, datadir=None):
    """Extract recent blocks as corpus inputs."""
    print(f"Extracting {count} recent blocks...")
    height_str = dash_cli("getblockcount", datadir=datadir)
    if not height_str:
        return 0

    height = int(height_str)
    saved = 0

    for h in range(max(0, height - count), height + 1):
        block_hash = dash_cli("getblockhash", str(h), datadir=datadir)
        if not block_hash:
            continue

        # Get serialized block
        block_hex = dash_cli("getblock", block_hash, "0", datadir=datadir)
        if block_hex:
            if save_corpus_input(output_dir, "block_deserialize", block_hex):
                saved += 1
            if save_corpus_input(output_dir, "block", block_hex):
                saved += 1

    print(f"  Saved {saved} block corpus inputs")
    return saved


def extract_special_txs(output_dir, count=100, datadir=None):
    """Extract special transactions (ProTx, etc.) from recent blocks."""
    print(f"Scanning {count} recent blocks for special transactions...")
    height_str = dash_cli("getblockcount", datadir=datadir)
    if not height_str:
        return 0

    height = int(height_str)
    saved = 0

    # Map special tx types to fuzz target names
    type_map = {
        1: "dash_proreg_tx",           # ProRegTx
        2: "dash_proupserv_tx",        # ProUpServTx
        3: "dash_proupreg_tx",         # ProUpRegTx
        4: "dash_prouprev_tx",         # ProUpRevTx
        5: "dash_cbtx",               # CbTx (coinbase)
        6: "dash_final_commitment_tx_payload",  # Quorum commitment
        7: "dash_mnhf_tx_payload",     # MN HF signal
        8: "dash_asset_lock_payload",  # Asset Lock
        9: "dash_asset_unlock_payload", # Asset Unlock
    }

    for h in range(max(0, height - count), height + 1):
        block_hash = dash_cli("getblockhash", str(h), datadir=datadir)
        if not block_hash:
            continue

        block_json = dash_cli("getblock", block_hash, "2", datadir=datadir)
        if not block_json:
            continue

        try:
            block = json.loads(block_json)
        except json.JSONDecodeError:
            continue

        for tx in block.get("tx", []):
            tx_type = tx.get("type", 0)
            if tx_type == 0:
                continue

            # Get raw transaction
            txid = tx.get("txid", "")
            raw_tx = dash_cli("getrawtransaction", txid, datadir=datadir)
            if not raw_tx:
                continue

            # Save full transaction
            if save_corpus_input(output_dir, "decode_tx", raw_tx):
                saved += 1

            # Extract special payload if we know the target
            extra_payload_size = tx.get("extraPayloadSize", 0)
            try:
                extra_payload_size = int(extra_payload_size)
            except (TypeError, ValueError):
                extra_payload_size = 0

            if extra_payload_size > 0 and tx_type in type_map:
                payload_hex, err = extract_extra_payload_hex(raw_tx, extra_payload_size)
                if not payload_hex:
                    print(
                        f"WARNING: Skipping special payload for tx {txid}: {err}",
                        file=sys.stderr,
                    )
                    continue

                target = type_map[tx_type]
                # Save payload bytes for both deserialize and roundtrip variants.
                for suffix in ["_deserialize", "_roundtrip"]:
                    if save_corpus_input(output_dir, f"{target}{suffix}", payload_hex):
                        saved += 1

    print(f"  Saved {saved} special transaction corpus inputs")
    return saved


def extract_governance_objects(output_dir, datadir=None):
    """Extract governance objects (proposals, triggers)."""
    print("Extracting governance objects...")
    result = dash_cli("gobject", "list", "all", datadir=datadir)
    if not result:
        return 0

    saved = 0
    try:
        objects = json.loads(result)
        for _obj_hash, obj_data in objects.items():
            data_hex = obj_data.get("DataHex", "")
            if data_hex:
                if save_corpus_input(output_dir, "dash_governance_object_deserialize", data_hex):
                    saved += 1
                if save_corpus_input(output_dir, "dash_governance_object_roundtrip", data_hex):
                    saved += 1
    except (json.JSONDecodeError, AttributeError):
        pass

    print(f"  Saved {saved} governance corpus inputs")
    return saved


def extract_masternode_list(output_dir, datadir=None):
    """Extract masternode list entries."""
    print("Extracting masternode list data...")
    result = dash_cli("protx", "list", "registered", "true", datadir=datadir)
    if not result:
        return 0

    saved = 0
    try:
        mn_list = json.loads(result)
        for mn in mn_list:
            protx_hash = mn.get("proTxHash", "")
            if not protx_hash:
                continue

            raw_tx = dash_cli("getrawtransaction", protx_hash, datadir=datadir)
            if not raw_tx:
                continue

            # Save full raw tx for full-transaction targets
            if save_corpus_input(output_dir, "decode_tx", raw_tx):
                saved += 1

            # Extract the special payload for payload-specific targets
            # ProRegTx type is 1, get extraPayloadSize from verbose tx
            verbose_tx = dash_cli("getrawtransaction", protx_hash, "true", datadir=datadir)
            if not verbose_tx:
                continue
            try:
                tx_info = json.loads(verbose_tx)
            except json.JSONDecodeError:
                continue

            extra_payload_size = tx_info.get("extraPayloadSize", 0)
            try:
                extra_payload_size = int(extra_payload_size)
            except (TypeError, ValueError):
                extra_payload_size = 0

            if extra_payload_size > 0:
                payload_hex, err = extract_extra_payload_hex(raw_tx, extra_payload_size)
                if payload_hex:
                    for target in ["dash_proreg_tx_deserialize", "dash_proreg_tx_roundtrip"]:
                        if save_corpus_input(output_dir, target, payload_hex):
                            saved += 1
                else:
                    print(f"WARNING: Could not extract payload from protx {protx_hash}: {err}", file=sys.stderr)
    except (json.JSONDecodeError, AttributeError):
        pass

    print(f"  Saved {saved} masternode corpus inputs")
    return saved


def extract_quorum_info(output_dir, datadir=None):
    """Extract quorum-related data from the chain.

    Note: quorum snapshot deserialize targets expect binary-serialized
    CQuorumSnapshot data, not JSON. We extract final commitment transactions
    from blocks instead, which are already captured by extract_special_txs()
    for type 6 (TRANSACTION_QUORUM_COMMITMENT). This function focuses on
    extracting quorum memberof data as raw bytes for other quorum targets.
    """
    print("Extracting quorum data...")
    result = dash_cli("quorum", "list", datadir=datadir)
    if not result:
        return 0

    saved = 0
    try:
        quorum_list = json.loads(result)
        for qtype, hashes in quorum_list.items():
            for qhash in hashes[:5]:  # Limit per type
                # Get the quorum commitment transaction via selectquorum
                # which gives us the quorumHash we can look up in blocks
                qinfo_str = dash_cli("quorum", "info", qtype, qhash, datadir=datadir)
                if not qinfo_str:
                    continue
                try:
                    qinfo = json.loads(qinfo_str)
                except json.JSONDecodeError:
                    continue
                # Extract the commitment tx if available
                mining_hash = qinfo.get("minedBlock", "")
                if mining_hash:
                    block_hex = dash_cli("getblock", mining_hash, "0", datadir=datadir)
                    if block_hex and save_corpus_input(output_dir, "block_deserialize", block_hex):
                        saved += 1
    except (json.JSONDecodeError, AttributeError):
        pass

    print(f"  Saved {saved} quorum corpus inputs")
    return saved


#
# These Dash-specific target names are forward-looking: corresponding fuzz targets
# are planned for a future PR. We pre-generate seeds now so coverage is ready as
# soon as those targets land.
def create_synthetic_seeds(output_dir):
    """Create minimal synthetic seed inputs for targets without chain data."""
    print("Creating synthetic seed inputs...")
    saved = 0

    # Targets that need synthetic seeds (serialized structs with known formats)
    synthetic_seeds = {
        # CoinJoin messages — minimal valid-ish payloads
        "dash_coinjoin_accept_deserialize": [
            "00000000" + "00" * 4,  # nDenom(4) + txCollateral
        ],
        "dash_coinjoin_queue_deserialize": [
            "00000000" + "00" * 48 + "00" * 96 + "0000000000000000",  # nDenom + proTxHash + vchSig + nTime
        ],
        "dash_coinjoin_status_update_deserialize": [
            "00000000" + "00000000" + "00000000",  # nSessionID + nState + nStatusUpdate
        ],
        # LLMQ messages
        "dash_recovered_sig_deserialize": [
            "64" + "00" * 32 + "00" * 32 + "00" * 96,  # llmqType + quorumHash + id + sig
        ],
        "dash_sig_ses_ann_deserialize": [
            "64" + "00" * 32 + "00000000" + "00" * 32,  # llmqType + quorumHash + nSessionId + id
        ],
        "dash_sig_share_deserialize": [
            "64" + "00" * 32 + "00000000" + "00" * 32 + "0000" + "00" * 96,
        ],
        # MNAuth
        "dash_mnauth_deserialize": [
            "00" * 32 + "00" * 32 + "00" * 96,  # proRegTxHash + signChallenge + sig
        ],
        # DKG messages
        "dash_dkg_complaint_deserialize": [
            "64" + "00" * 32 + "00" * 32 + "0000" + "00",  # minimal
        ],
    }

    for target, seeds in synthetic_seeds.items():
        for seed_hex in seeds:
            if save_corpus_input(output_dir, target, seed_hex):
                saved += 1
            # Also save roundtrip variant
            roundtrip_target = target.replace("_deserialize", "_roundtrip")
            if save_corpus_input(output_dir, roundtrip_target, seed_hex):
                saved += 1

    print(f"  Created {saved} synthetic seed inputs")
    return saved


def main():
    parser = argparse.ArgumentParser(
        description="Extract seed corpus from a running Dash node for fuzz testing"
    )
    parser.add_argument(
        "--output-dir", "-o",
        required=True,
        help="Output directory for corpus files"
    )
    parser.add_argument(
        "--datadir",
        help="Dash data directory (passed to dash-cli)"
    )
    parser.add_argument(
        "--blocks", type=int, default=100,
        help="Number of recent blocks to scan (default: 100)"
    )
    parser.add_argument(
        "--synthetic-only",
        action="store_true",
        help="Only generate synthetic seeds (no RPC required)"
    )
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    total = 0

    if not args.synthetic_only:
        total += extract_blocks(output_dir, count=args.blocks, datadir=args.datadir)
        total += extract_special_txs(output_dir, count=args.blocks, datadir=args.datadir)
        total += extract_governance_objects(output_dir, datadir=args.datadir)
        total += extract_masternode_list(output_dir, datadir=args.datadir)
        total += extract_quorum_info(output_dir, datadir=args.datadir)

    total += create_synthetic_seeds(output_dir)

    print(f"\nTotal: {total} corpus inputs saved to {output_dir}")

    # Print summary
    print("\nCorpus directory summary:")
    for target_dir in sorted(output_dir.iterdir()):
        if target_dir.is_dir():
            file_count = len(list(target_dir.iterdir()))
            print(f"  {target_dir.name}: {file_count} files")


if __name__ == "__main__":
    main()
