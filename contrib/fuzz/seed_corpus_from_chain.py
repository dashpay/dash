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


# Must match src/version.h PROTOCOL_VERSION. Several fuzz harnesses read a
# 4-byte little-endian int from the start of the buffer and use it as the
# stream version before deserializing the object:
#   * The Dash-specific helpers DashDeserializeFromFuzzingInput /
#     DashRoundtripFromFuzzingInput (src/test/fuzz/deserialize_dash.cpp,
#     src/test/fuzz/roundtrip_dash.cpp), used by dash_*_deserialize and
#     dash_*_roundtrip targets.
#   * The upstream block target (src/test/fuzz/block.cpp), which reads the
#     version int inline before deserializing CBlock.
#   * The upstream block_deserialize target (src/test/fuzz/deserialize.cpp),
#     via DeserializeFromFuzzingInput when no explicit protocol_version is
#     passed.
# Chain data we extract is serialized at PROTOCOL_VERSION, so we prepend
# that value to seeds for those targets.
STREAM_VERSION = 70240
STREAM_VERSION_PREFIX = STREAM_VERSION.to_bytes(4, byteorder="little", signed=False)

# Non-Dash targets (outside the dash_* naming convention) whose harnesses
# also consume the 4-byte stream version prefix described above.
_NON_DASH_STREAM_VERSION_TARGETS = frozenset({"block", "block_deserialize", "blockmerkleroot"})


def _needs_stream_version_prefix(target_name):
    """Return True if this target's harness consumes a 4-byte stream version prefix.

    Matches the DashDeserializeFromFuzzingInput / DashRoundtripFromFuzzingInput
    helpers used by every dash_*_deserialize and dash_*_roundtrip target, plus
    the upstream ``block`` and ``block_deserialize`` targets which do the same
    (see src/test/fuzz/block.cpp and src/test/fuzz/deserialize.cpp). Other
    non-Dash targets (decode_tx, ...) don't consume such a prefix and must be
    left untouched.
    """
    if target_name in _NON_DASH_STREAM_VERSION_TARGETS:
        return True
    return target_name.startswith("dash_") and (
        target_name.endswith("_deserialize") or target_name.endswith("_roundtrip")
    )


def save_corpus_input(output_dir, target_name, data_hex):
    """Save a hex-encoded blob as a corpus input file."""
    target_dir = output_dir / target_name
    target_dir.mkdir(parents=True, exist_ok=True)

    try:
        raw_bytes = bytes.fromhex(data_hex)
    except ValueError:
        print(f"WARNING: Invalid hex data for target {target_name}, skipping", file=sys.stderr)
        return False

    if _needs_stream_version_prefix(target_name):
        raw_bytes = STREAM_VERSION_PREFIX + raw_bytes

    filename = hashlib.sha256(raw_bytes).hexdigest()[:16]
    filepath = target_dir / filename

    if not filepath.exists():
        filepath.write_bytes(raw_bytes)
        return True
    return False


def _compact_size(n):
    """Encode an unsigned integer as a Bitcoin/Dash CompactSize (aka VarInt)."""
    if n < 253:
        return bytes([n])
    if n < 0x10000:
        return b"\xfd" + n.to_bytes(2, "little")
    if n < 0x100000000:
        return b"\xfe" + n.to_bytes(4, "little")
    return b"\xff" + n.to_bytes(8, "little")


def _var_bytes(b):
    """CompactSize length prefix + the bytes themselves (vector<unsigned char>)."""
    return _compact_size(len(b)) + b


def _uint256_from_hex(h):
    """Convert an RPC-form uint256 hex string (big-endian display) to its 32-byte
    wire representation (little-endian internal). Empty/missing input -> zero hash.
    """
    if not h:
        return b"\x00" * 32
    raw = bytes.fromhex(h)
    if len(raw) != 32:
        raise ValueError(f"uint256 hex must be 32 bytes, got {len(raw)}")
    return raw[::-1]


def _parse_outpoint_short(s):
    """Parse a masternode-outpoint short form 'txid-n' into wire bytes
    (uint256 hash + uint32 n). Returns zeroed outpoint on missing/invalid input.
    RPC exposes the signing masternode outpoint as "txid-n" (COutPoint::ToStringShort)."""
    if not s or "-" not in s:
        return b"\x00" * 32 + (0).to_bytes(4, "little")
    txid, _, idx = s.rpartition("-")
    try:
        n = int(idx)
    except ValueError:
        return b"\x00" * 32 + (0).to_bytes(4, "little")
    try:
        return _uint256_from_hex(txid) + n.to_bytes(4, "little")
    except ValueError:
        return b"\x00" * 32 + (0).to_bytes(4, "little")


def serialize_governance_object(obj_data):
    """Serialize a structurally valid Governance::Object from a `gobject list` RPC entry.

    The fuzz target ``dash_governance_object_common_deserialize`` reads a full
    Governance::Object (see src/governance/common.h SERIALIZE_METHODS). The RPC
    exposes only a subset of the fields; the rest are filled with documented
    best-effort defaults so the seed is structurally sound rather than random bytes.

    Field sources:
      * ``hashParent``      — not exposed by RPC (root-object semantics); zeroed.
      * ``revision``        — not exposed by RPC; defaulted to 1.
      * ``time``            — from ``CreationTime`` (int64 seconds).
      * ``collateralHash``  — from ``CollateralHash`` (hex, RPC display order).
      * ``vchData``         — from ``DataHex`` (raw payload bytes).
      * ``type``            — from ``ObjectType`` (int: UNKNOWN=0, PROPOSAL=1, TRIGGER=2).
      * ``masternodeOutpoint`` — from ``SigningMasternode`` ("txid-n"), if present.
      * ``vchSig``          — not exposed by RPC; left empty.
    """
    hash_parent = b"\x00" * 32
    revision = 1
    try:
        time_ = int(obj_data.get("CreationTime", 0))
    except (TypeError, ValueError):
        time_ = 0
    try:
        collateral_hash = _uint256_from_hex(obj_data.get("CollateralHash", ""))
    except ValueError:
        collateral_hash = b"\x00" * 32

    data_hex = obj_data.get("DataHex", "") or ""
    try:
        vch_data = bytes.fromhex(data_hex)
    except ValueError:
        vch_data = b""

    try:
        obj_type = int(obj_data.get("ObjectType", 0))
    except (TypeError, ValueError):
        obj_type = 0

    outpoint = _parse_outpoint_short(obj_data.get("SigningMasternode", ""))
    vch_sig = b""

    return (
        hash_parent
        + revision.to_bytes(4, "little", signed=True)
        + time_.to_bytes(8, "little", signed=True)
        + collateral_hash
        + _var_bytes(vch_data)
        + obj_type.to_bytes(4, "little", signed=True)
        + outpoint
        + _var_bytes(vch_sig)
    )


def serialize_quorum_data_request(llmq_type, quorum_hash_hex, pro_tx_hash_hex="", n_data_mask=3):
    """Serialize a llmq::CQuorumDataRequest (src/llmq/quorums.h).

    Wire layout: llmqType (uint8) + quorumHash (uint256) + nDataMask (uint16 LE)
    + proTxHash (uint256) + nError (uint8, NONE=0). nError is optional per the
    SERIALIZE_METHODS (try/catch on read, conditional on write) — we include it
    with NONE so the seed round-trips cleanly.

    nDataMask defaults to QUORUM_VERIFICATION_VECTOR|ENCRYPTED_CONTRIBUTIONS (0x3)
    for maximal coverage; RPC doesn't expose a concrete in-flight request.
    """
    return (
        bytes([llmq_type & 0xFF])
        + _uint256_from_hex(quorum_hash_hex)
        + (n_data_mask & 0xFFFF).to_bytes(2, "little")
        + _uint256_from_hex(pro_tx_hash_hex)
        + bytes([0])  # nError = NONE
    )


def serialize_get_quorum_rotation_info(block_request_hash_hex, base_block_hashes_hex, extra_share=False):
    """Serialize a llmq::CGetQuorumRotationInfo (src/llmq/snapshot.h).

    Wire layout: vector<uint256> baseBlockHashes + uint256 blockRequestHash + bool extraShare.
    """
    out = _compact_size(len(base_block_hashes_hex))
    for h in base_block_hashes_hex:
        out += _uint256_from_hex(h)
    out += _uint256_from_hex(block_request_hash_hex)
    out += bytes([1 if extra_share else 0])
    return out


def serialize_quorum_snapshot(active_quorum_members, skip_list, mn_skip_list_mode=0):
    """Serialize a llmq::CQuorumSnapshot (src/llmq/snapshot.h).

    Wire layout: mnSkipListMode (int, 4 bytes LE — SnapshotSkipMode enum underlying type is int)
    + CompactSize(len(activeQuorumMembers)) + WriteFixedBitSet(activeQuorumMembers)
    + CompactSize(len(mnSkipList)) + int32 LE for each skip-list entry.
    """
    count = len(active_quorum_members)
    out = mn_skip_list_mode.to_bytes(4, "little", signed=True)
    out += _compact_size(count)

    nbytes = (count + 7) // 8
    buf = bytearray(nbytes)
    for i, v in enumerate(active_quorum_members):
        if v:
            buf[i // 8] |= 1 << (i % 8)
    out += bytes(buf)

    out += _compact_size(len(skip_list))
    for x in skip_list:
        out += int(x).to_bytes(4, "little", signed=True)
    return out


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
            if save_corpus_input(output_dir, "blockmerkleroot", block_hex):
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
            raw_tx = dash_cli("getrawtransaction", txid, "false", block_hash, datadir=datadir)
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
    """Extract governance objects (proposals, triggers) into structurally valid seeds.

    The fuzz target deserializes a full Governance::Object, not raw payload bytes,
    so we reconstruct the object from the RPC fields (CollateralHash, CreationTime,
    ObjectType, SigningMasternode, DataHex) and synthesize best-effort defaults for
    fields the RPC doesn't expose (hashParent, revision, vchSig). CGovernanceObject's
    non-disk serialization is just ``m_obj`` (see src/governance/object.h:248) so the
    same bytes are valid seeds for dash_governance_object_{deserialize,roundtrip}.
    """
    print("Extracting governance objects...")
    result = dash_cli("gobject", "list", "all", datadir=datadir)
    if not result:
        return 0

    saved = 0
    try:
        objects = json.loads(result)
    except (json.JSONDecodeError, AttributeError):
        return 0

    if not isinstance(objects, dict):
        return 0

    targets = [
        "dash_governance_object_common_deserialize",
        "dash_governance_object_deserialize",
        "dash_governance_object_roundtrip",
    ]

    for obj_hash, obj_data in objects.items():
        if not isinstance(obj_data, dict):
            continue
        try:
            serialized = serialize_governance_object(obj_data)
        except (ValueError, OverflowError) as e:
            print(f"WARNING: Skipping governance object {obj_hash}: {e}", file=sys.stderr)
            continue
        seed_hex = serialized.hex()
        for target in targets:
            if save_corpus_input(output_dir, target, seed_hex):
                saved += 1

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

            state = mn.get("state")
            if not isinstance(state, dict):
                print(f"WARNING: Missing state for protx {protx_hash}, skipping", file=sys.stderr)
                continue

            registered_height = state.get("registeredHeight")
            try:
                registered_height = int(registered_height)
            except (TypeError, ValueError):
                print(
                    f"WARNING: Invalid registeredHeight for protx {protx_hash}: {registered_height!r}",
                    file=sys.stderr,
                )
                continue

            block_hash = dash_cli("getblockhash", str(registered_height), datadir=datadir)
            if not block_hash:
                continue

            block_json = dash_cli("getblock", block_hash, "1", datadir=datadir)
            if not block_json:
                continue

            try:
                block = json.loads(block_json)
            except json.JSONDecodeError:
                continue

            if protx_hash not in block.get("tx", []):
                print(
                    f"WARNING: ProRegTx {protx_hash} not found in registeredHeight block {registered_height} ({block_hash}), skipping",
                    file=sys.stderr,
                )
                continue

            raw_tx = dash_cli("getrawtransaction", protx_hash, "false", block_hash, datadir=datadir)
            if not raw_tx:
                continue

            # Save full raw tx for full-transaction targets
            if save_corpus_input(output_dir, "decode_tx", raw_tx):
                saved += 1

            # Extract the special payload for payload-specific targets
            # ProRegTx type is 1, get extraPayloadSize from verbose tx
            verbose_tx = dash_cli("getrawtransaction", protx_hash, "true", block_hash, datadir=datadir)
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
    """Seed the quorum-specific P2P/message fuzz targets from live chain data.

    Drives three fuzz-target families (each with _deserialize and _roundtrip):
      * dash_quorum_data_request_*        — CQuorumDataRequest (QGETDATA message)
      * dash_get_quorum_rotation_info_*   — CGetQuorumRotationInfo (QGETRTINFO message)
      * dash_quorum_snapshot_*            — CQuorumSnapshot (QRINFO payload piece)

    We derive inputs from ``quorum list`` + ``quorum info`` + ``quorum rotationinfo``:

      * CQuorumDataRequest: if ``quorum info`` exposes a real member proTxHash we
        use it with nDataMask=3 (VV|EC); otherwise we fall back to a zero proTxHash
        with nDataMask=1 (verification-vector only). Both shapes are structurally
        valid per the protocol.
      * CQuorumSnapshot: prefer snapshots returned by ``quorum rotationinfo``
        (``quorumSnapshotAtHMinus{C,2C,3C}`` and ``quorumSnapshotList``), whose
        ``activeQuorumMembers`` / ``mnSkipListMode`` / ``mnSkipList`` fields are
        serialized verbatim. Fall back to the ``quorum info`` member-validity
        bitmap with an empty skip list + MODE_NO_SKIPPING if rotationinfo is
        unavailable for that quorum.
      * CGetQuorumRotationInfo: serialize the exact requests that rotationinfo
        actually answered (blockRequestHash == quorumHash, empty baseBlockHashes,
        extraShare=false). Only fall back to a tip-hash + quorum-hash composite
        when no rotationinfo request succeeded.
    """
    print("Extracting quorum data...")
    result = dash_cli("quorum", "list", datadir=datadir)
    if not result:
        return 0

    # quorum info expects a numeric llmqType, but quorum list returns string keys
    llmq_type_map = {
        "llmq_50_60": 1,
        "llmq_400_60": 2,
        "llmq_400_85": 3,
        "llmq_100_67": 4,
        "llmq_60_75": 5,
        "llmq_25_67": 6,
        "llmq_test": 100,
        "llmq_devnet": 101,
        "llmq_test_v17": 102,
        "llmq_test_dip0024": 103,
        "llmq_test_instantsend": 104,
        "llmq_devnet_dip0024": 105,
        "llmq_test_platform": 106,
        "llmq_devnet_platform": 107,
    }

    saved = 0
    try:
        quorum_list = json.loads(result)
    except (json.JSONDecodeError, AttributeError):
        return 0

    if not isinstance(quorum_list, dict):
        return 0

    # Collect quorum hashes across types (used as a fallback for
    # CGetQuorumRotationInfo seeds if no rotationinfo request succeeds).
    all_quorum_hashes = []
    # Track the exact (blockRequestHash, baseBlockHashes, extraShare) tuples
    # for which we successfully invoked `quorum rotationinfo` — these become
    # the preferred CGetQuorumRotationInfo seeds.
    successful_rotation_requests = []

    for qtype, hashes in quorum_list.items():
        numeric_type = llmq_type_map.get(qtype)
        if numeric_type is None:
            print(f"WARNING: Unknown quorum type '{qtype}', skipping", file=sys.stderr)
            continue
        if not isinstance(hashes, list):
            continue

        for qhash in hashes[:5]:  # Limit per type
            if not isinstance(qhash, str) or not qhash:
                continue
            all_quorum_hashes.append(qhash)

            # Fetch quorum info once up-front so we can both pull a real
            # member proTxHash for CQuorumDataRequest and use its
            # member-validity bitmap as a CQuorumSnapshot fallback.
            qinfo = None
            qinfo_str = dash_cli("quorum", "info", str(numeric_type), qhash, datadir=datadir)
            if qinfo_str:
                try:
                    parsed = json.loads(qinfo_str)
                except json.JSONDecodeError:
                    parsed = None
                if isinstance(parsed, dict):
                    qinfo = parsed

            # --- Seed dash_quorum_data_request_{deserialize,roundtrip} ---
            # If we have a real member proTxHash, emit a VV|EC request (mask=3);
            # otherwise emit a zero-proTxHash VV-only request (mask=1). Both
            # match the CQuorumDataRequest protocol surface.
            member_protx = ""
            if qinfo:
                members = qinfo.get("members", [])
                if isinstance(members, list):
                    for m in members:
                        if not isinstance(m, dict):
                            continue
                        p = m.get("proTxHash", "")
                        if isinstance(p, str) and p:
                            member_protx = p
                            break
            n_data_mask = 3 if member_protx else 1
            try:
                qdr = serialize_quorum_data_request(
                    numeric_type, qhash, member_protx, n_data_mask=n_data_mask
                )
            except ValueError as e:
                print(f"WARNING: Skipping CQuorumDataRequest seed for {qhash}: {e}", file=sys.stderr)
            else:
                seed_hex = qdr.hex()
                for target in [
                    "dash_quorum_data_request_deserialize",
                    "dash_quorum_data_request_roundtrip",
                ]:
                    if save_corpus_input(output_dir, target, seed_hex):
                        saved += 1

            # --- Seed dash_quorum_snapshot_{deserialize,roundtrip} ---
            # Prefer real rotationinfo snapshots when available.
            snapshot_seeded = False
            rot_str = dash_cli("quorum", "rotationinfo", qhash, "false", datadir=datadir)
            rot_info = None
            if rot_str:
                try:
                    parsed = json.loads(rot_str)
                except json.JSONDecodeError:
                    parsed = None
                if isinstance(parsed, dict):
                    rot_info = parsed
                    successful_rotation_requests.append((qhash, [], False))

            if rot_info is not None:
                snapshot_candidates = []
                for key in (
                    "quorumSnapshotAtHMinusC",
                    "quorumSnapshotAtHMinus2C",
                    "quorumSnapshotAtHMinus3C",
                ):
                    s = rot_info.get(key)
                    if isinstance(s, dict):
                        snapshot_candidates.append(s)
                snap_list = rot_info.get("quorumSnapshotList", [])
                if isinstance(snap_list, list):
                    snapshot_candidates.extend(s for s in snap_list if isinstance(s, dict))

                for snap in snapshot_candidates:
                    active_raw = snap.get("activeQuorumMembers", [])
                    skip_list_raw = snap.get("mnSkipList", [])
                    skip_mode_raw = snap.get("mnSkipListMode", 0)
                    if not isinstance(active_raw, list):
                        continue
                    if not isinstance(skip_list_raw, list):
                        skip_list_raw = []
                    try:
                        skip_mode_int = int(skip_mode_raw)
                    except (TypeError, ValueError):
                        skip_mode_int = 0
                    try:
                        snap_bytes = serialize_quorum_snapshot(
                            [bool(x) for x in active_raw],
                            [int(x) for x in skip_list_raw],
                            mn_skip_list_mode=skip_mode_int,
                        )
                    except (ValueError, OverflowError, TypeError) as e:
                        print(
                            f"WARNING: Skipping rotationinfo CQuorumSnapshot seed for {qhash}: {e}",
                            file=sys.stderr,
                        )
                        continue
                    seed_hex = snap_bytes.hex()
                    for target in [
                        "dash_quorum_snapshot_deserialize",
                        "dash_quorum_snapshot_roundtrip",
                    ]:
                        if save_corpus_input(output_dir, target, seed_hex):
                            saved += 1
                    snapshot_seeded = True

            # Fall back to the quorum-info-derived approximation only when
            # rotationinfo didn't yield a usable snapshot.
            if not snapshot_seeded and qinfo:
                members = qinfo.get("members", [])
                if isinstance(members, list) and members:
                    active = [bool(m.get("valid", False)) for m in members if isinstance(m, dict)]
                    try:
                        snap = serialize_quorum_snapshot(active, [])
                    except (ValueError, OverflowError) as e:
                        print(
                            f"WARNING: Skipping CQuorumSnapshot seed for {qhash}: {e}",
                            file=sys.stderr,
                        )
                    else:
                        seed_hex = snap.hex()
                        for target in [
                            "dash_quorum_snapshot_deserialize",
                            "dash_quorum_snapshot_roundtrip",
                        ]:
                            if save_corpus_input(output_dir, target, seed_hex):
                                saved += 1

    # --- Seed dash_get_quorum_rotation_info_{deserialize,roundtrip} ---
    # Prefer the exact requests rotationinfo actually answered. Each is a
    # minimal (blockRequestHash, [], extraShare=false) tuple matching the
    # CLI call we issued above.
    variants = []
    if successful_rotation_requests:
        for block_request_hash, base_hashes, extra_share in successful_rotation_requests[:8]:
            try:
                variants.append(
                    serialize_get_quorum_rotation_info(
                        block_request_hash, base_hashes, extra_share=extra_share
                    )
                )
            except ValueError as e:
                print(f"WARNING: Skipping CGetQuorumRotationInfo seed: {e}", file=sys.stderr)
    elif all_quorum_hashes:
        # Fallback when no rotationinfo call succeeded: synthesize tip-hash
        # + quorum-hash variants so the corpus isn't empty.
        tip_hash = dash_cli("getbestblockhash", datadir=datadir)
        if tip_hash:
            try:
                variants.append(
                    serialize_get_quorum_rotation_info(
                        tip_hash, all_quorum_hashes[:8], extra_share=False
                    )
                )
                variants.append(
                    serialize_get_quorum_rotation_info(
                        tip_hash, all_quorum_hashes[:1], extra_share=True
                    )
                )
            except ValueError as e:
                print(f"WARNING: Skipping CGetQuorumRotationInfo fallback seeds: {e}", file=sys.stderr)
                variants = []

    for seed in variants:
        seed_hex = seed.hex()
        for target in [
            "dash_get_quorum_rotation_info_deserialize",
            "dash_get_quorum_rotation_info_roundtrip",
        ]:
            if save_corpus_input(output_dir, target, seed_hex):
                saved += 1

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
