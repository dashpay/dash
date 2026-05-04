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
import os
import re
import subprocess
import sys
import tempfile
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


def _read_protocol_version():
    """Read PROTOCOL_VERSION from src/version.h."""
    version_header = Path(__file__).resolve().parents[2] / "src" / "version.h"
    match = re.search(
        r"static\s+const\s+int\s+PROTOCOL_VERSION\s*=\s*(\d+)\s*;",
        version_header.read_text(encoding="utf-8"),
    )
    if not match:
        raise RuntimeError(f"Could not parse PROTOCOL_VERSION from {version_header}")
    return int(match.group(1))


# The stream version is needed by several fuzz harnesses that read a 4-byte
# little-endian int from the start of the buffer and use it as the stream
# version before deserializing the object:
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
# that value to seeds for those targets. The lookup is deferred to first use
# so `--help` and callers that supply --stream-version / DASH_FUZZ_STREAM_VERSION
# don't require an in-tree src/version.h.
_STREAM_VERSION_OVERRIDE = None
_STREAM_VERSION_CACHE = None


def _resolve_stream_version():
    """Return the stream version, preferring an explicit override and falling back
    to parsing src/version.h. Cached after first successful resolution."""
    global _STREAM_VERSION_CACHE
    if _STREAM_VERSION_CACHE is not None:
        return _STREAM_VERSION_CACHE
    if _STREAM_VERSION_OVERRIDE is not None:
        _STREAM_VERSION_CACHE = _STREAM_VERSION_OVERRIDE
        return _STREAM_VERSION_CACHE
    env_override = os.environ.get("DASH_FUZZ_STREAM_VERSION")
    if env_override:
        try:
            _STREAM_VERSION_CACHE = int(env_override)
        except ValueError as e:
            raise RuntimeError(
                f"DASH_FUZZ_STREAM_VERSION must be an integer, got {env_override!r}"
            ) from e
        return _STREAM_VERSION_CACHE
    _STREAM_VERSION_CACHE = _read_protocol_version()
    return _STREAM_VERSION_CACHE


def _stream_version_prefix():
    return _resolve_stream_version().to_bytes(4, byteorder="little", signed=False)

# Non-Dash targets (outside the dash_* naming convention) whose harnesses
# also consume the 4-byte stream version prefix described above.
_NON_DASH_STREAM_VERSION_TARGETS = frozenset({"block", "block_deserialize", "blockmerkleroot"})
_DESERIALIZE_ONLY_DASH_TARGETS = frozenset(
    {
        "dash_governance_vote_deserialize",
        "dash_vote_instance_deserialize",
        "dash_vote_rec_deserialize",
        "dash_governance_vote_file_deserialize",
        "dash_mnhf_tx_deserialize",
    }
)


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
        raw_bytes = _stream_version_prefix() + raw_bytes

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


def _var_list(items):
    """CompactSize length prefix + concatenated serialized entries."""
    return _compact_size(len(items)) + b"".join(items)


def _serialize_int32(value):
    return int(value).to_bytes(4, "little", signed=True)


def _serialize_uint32(value):
    return int(value).to_bytes(4, "little", signed=False)


def _serialize_int64(value):
    return int(value).to_bytes(8, "little", signed=True)


def _serialize_uint64(value):
    return int(value).to_bytes(8, "little", signed=False)


def _serialize_bool(value):
    return bytes([1 if value else 0])


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


def _serialize_outpoint(txid_hex="", n=0):
    return _uint256_from_hex(txid_hex) + _serialize_uint32(n)


def _serialize_dynbitset(bits):
    count = len(bits)
    nbytes = (count + 7) // 8
    buf = bytearray(nbytes)
    for i, value in enumerate(bits):
        if value:
            buf[i // 8] |= 1 << (i % 8)
    return _compact_size(count) + bytes(buf)


def _serialize_string(value):
    return _var_bytes(value.encode("utf-8"))


def _serialize_txin(prev_txid_hex="", prev_n=0, script_sig=b"", sequence=0xFFFFFFFF):
    return _serialize_outpoint(prev_txid_hex, prev_n) + _var_bytes(script_sig) + _serialize_uint32(sequence)


def _serialize_txout(value=0, script_pubkey=b""):
    return int(value).to_bytes(8, "little", signed=True) + _var_bytes(script_pubkey)


def _serialize_transaction(vin=None, vout=None, n_version=2, n_type=0, n_lock_time=0, extra_payload=b""):
    vin = list(vin or [])
    vout = list(vout or [])
    n32bit_version = (int(n_version) & 0xFFFF) | ((int(n_type) & 0xFFFF) << 16)
    out = _serialize_uint32(n32bit_version)
    out += _var_list(vin)
    out += _var_list(vout)
    out += _serialize_uint32(n_lock_time)
    if n_version >= 3 and n_type != 0:
        out += _var_bytes(extra_payload)
    return out


def _final_commitment_from_tx_payload(payload_hex):
    """Extract the embedded CFinalCommitment from a CFinalCommitmentTxPayload."""
    try:
        payload = bytes.fromhex(payload_hex)
    except ValueError as e:
        raise ValueError("payload is not valid hex") from e
    if len(payload) < 6:
        raise ValueError("payload too short for CFinalCommitmentTxPayload header")
    return payload[6:]


_GOVERNANCE_OUTCOME_NAME_TO_INT = {
    "none": 0,
    "yes": 1,
    "no": 2,
    "abstain": 3,
}

_GOVERNANCE_SIGNAL_NAME_TO_INT = {
    "none": 0,
    "funding": 1,
    "valid": 2,
    "delete": 3,
    "endorsed": 4,
}


def parse_governance_vote_record(vote_record, parent_hash_hex):
    """Parse one `gobject getcurrentvotes` string into CGovernanceVote fields."""
    if not isinstance(vote_record, str):
        raise ValueError("vote record must be a string")
    parts = vote_record.split(":")
    if len(parts) != 5:
        raise ValueError(f"unexpected governance vote format: {vote_record!r}")
    outpoint_text, timestamp_text, outcome_text, signal_text, _vote_weight = parts
    if "-" not in outpoint_text:
        raise ValueError(f"unexpected masternode outpoint format: {outpoint_text!r}")
    txid_text, _, n_text = outpoint_text.rpartition("-")
    try:
        timestamp = int(timestamp_text)
    except ValueError as e:
        raise ValueError(f"invalid governance vote timestamp: {timestamp_text!r}") from e
    outcome = _GOVERNANCE_OUTCOME_NAME_TO_INT.get(outcome_text.lower())
    if outcome is None:
        raise ValueError(f"unknown governance vote outcome: {outcome_text!r}")
    signal = _GOVERNANCE_SIGNAL_NAME_TO_INT.get(signal_text.lower())
    if signal is None:
        raise ValueError(f"unknown governance vote signal: {signal_text!r}")
    return {
        "masternode_txid": txid_text,
        "masternode_n": int(n_text),
        "parent_hash": parent_hash_hex,
        "outcome": outcome,
        "signal": signal,
        "timestamp": timestamp,
        "signature": b"",
    }


def serialize_governance_vote(parsed_vote):
    """Serialize CGovernanceVote exactly as in src/governance/vote.h."""
    return (
        _serialize_outpoint(parsed_vote["masternode_txid"], parsed_vote["masternode_n"])
        + _uint256_from_hex(parsed_vote["parent_hash"])
        + _serialize_int32(parsed_vote["outcome"])
        + _serialize_int32(parsed_vote["signal"])
        + _serialize_int64(parsed_vote["timestamp"])
        + _var_bytes(parsed_vote.get("signature", b""))
    )


def serialize_vote_instance(outcome, updated_time, creation_time):
    """Serialize vote_instance_t exactly as in src/governance/object.h."""
    return _serialize_int32(outcome) + _serialize_int64(updated_time) + _serialize_int64(creation_time)


def serialize_vote_rec(signal_to_instance):
    """Serialize vote_rec_t (std::map<int, vote_instance_t>)."""
    items = []
    for signal, instance in sorted(signal_to_instance.items()):
        items.append(_serialize_int32(signal) + instance)
    return _var_list(items)


def serialize_governance_vote_file(votes):
    """Serialize CGovernanceObjectVoteFile from a list of serialized CGovernanceVote entries."""
    return _serialize_int32(len(votes)) + _var_list(votes)


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

                if tx_type == 6:
                    try:
                        commitment_hex = _final_commitment_from_tx_payload(payload_hex).hex()
                    except ValueError as e:
                        print(
                            f"WARNING: Skipping final commitment seed for tx {txid}: {e}",
                            file=sys.stderr,
                        )
                    else:
                        for suffix in ["_deserialize", "_roundtrip"]:
                            if save_corpus_input(output_dir, f"dash_final_commitment{suffix}", commitment_hex):
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
    vote_targets = ["dash_governance_vote_deserialize"]
    vote_instance_targets = ["dash_vote_instance_deserialize"]
    vote_rec_targets = ["dash_vote_rec_deserialize"]
    vote_file_targets = ["dash_governance_vote_file_deserialize"]

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

        votes_result = dash_cli("gobject", "getcurrentvotes", obj_hash, datadir=datadir)
        if not votes_result:
            continue
        try:
            votes_map = json.loads(votes_result)
        except (json.JSONDecodeError, AttributeError):
            continue
        if not isinstance(votes_map, dict):
            continue

        serialized_votes = []
        signal_to_instance = {}
        for vote_hash, vote_record in votes_map.items():
            del vote_hash  # Key is informational only; serialized vote recomputes its own hash.
            try:
                parsed_vote = parse_governance_vote_record(vote_record, obj_hash)
                serialized_vote = serialize_governance_vote(parsed_vote)
            except (ValueError, OverflowError) as e:
                print(f"WARNING: Skipping governance vote for {obj_hash}: {e}", file=sys.stderr)
                continue

            serialized_votes.append(serialized_vote)
            vote_hex = serialized_vote.hex()
            for target in vote_targets:
                if save_corpus_input(output_dir, target, vote_hex):
                    saved += 1

            updated_time = parsed_vote["timestamp"]
            instance = serialize_vote_instance(
                parsed_vote["outcome"], updated_time, updated_time
            )
            signal_to_instance[parsed_vote["signal"]] = instance
            instance_hex = instance.hex()
            for target in vote_instance_targets:
                if save_corpus_input(output_dir, target, instance_hex):
                    saved += 1

        if signal_to_instance:
            vote_rec_hex = serialize_vote_rec(signal_to_instance).hex()
            for target in vote_rec_targets:
                if save_corpus_input(output_dir, target, vote_rec_hex):
                    saved += 1

        if serialized_votes:
            vote_file_hex = serialize_governance_vote_file(serialized_votes).hex()
            for target in vote_file_targets:
                if save_corpus_input(output_dir, target, vote_file_hex):
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

    minimal_tx = _serialize_transaction()
    minimal_txin = _serialize_txin()
    minimal_txout = _serialize_txout()
    minimal_final_commitment = (
        (3).to_bytes(2, "little")
        + bytes([1])
        + b"\x00" * 32
        + _serialize_dynbitset([])
        + _serialize_dynbitset([])
        + b"\x00" * 48
        + b"\x00" * 32
        + b"\x00" * 96
        + b"\x00" * 96
    )
    minimal_governance_vote = serialize_governance_vote(
        {
            "masternode_txid": "00" * 32,
            "masternode_n": 0,
            "parent_hash": "00" * 32,
            "outcome": 1,
            "signal": 1,
            "timestamp": 0,
            "signature": b"",
        }
    )
    minimal_vote_instance = serialize_vote_instance(1, 0, 0)
    minimal_vote_rec = serialize_vote_rec({1: minimal_vote_instance})
    minimal_vote_file = serialize_governance_vote_file([minimal_governance_vote])
    minimal_bls_ies_blob = b"\x00" * 48 + b"\x00" * 32 + _var_bytes(b"seed")
    minimal_bls_ies_multi = b"\x00" * 48 + b"\x00" * 32 + _var_list([_var_bytes(b"seed0"), _var_bytes(b"seed1")])
    minimal_coinjoin_entry = _var_list([minimal_txin]) + minimal_tx + _var_list([minimal_txout])
    minimal_coinjoin_broadcast_tx = minimal_tx + b"\x00" * 32 + _var_bytes(b"") + _serialize_int64(0)
    minimal_premature_commitment = (
        bytes([1])
        + b"\x00" * 32
        + b"\x00" * 32
        + _serialize_dynbitset([True])
        + b"\x00" * 48
        + b"\x00" * 32
        + b"\x00" * 96
        + b"\x00" * 96
    )

    # CCreditPool (src/evo/creditpool.h):
    #   locked (int64) + currentLimit (int64) + latelyUnlocked (int64) + ranges (set<Range>)
    # An empty CRangesSet is just CompactSize(0) = 1 byte.
    minimal_credit_pool = (
        _serialize_int64(0)
        + _serialize_int64(0)
        + _serialize_int64(0)
        + _compact_size(0)
    )

    # CDeterministicMNState (src/evo/dmnstate.h) at nVersion=LegacyBLS=1:
    #   * pubKeyOperator is wrapped with legacy=true (48 bytes)
    #   * NetInfoSerWrapper(is_extended=false): MnNetInfo unserializes a CService
    #     whose wire format without ADDRV2_FORMAT is V1 (16-byte IPv6 addr + 2-byte BE port).
    #   * platformP2PPort/HTTPPort are emitted only when nVersion < ExtAddr=3.
    minimal_dmn_state = (
        _serialize_int32(1)         # nVersion = LegacyBLS
        + _serialize_int32(0)       # nRegisteredHeight
        + _serialize_int32(0)       # nLastPaidHeight
        + _serialize_int32(0)       # nConsecutivePayments
        + _serialize_int32(0)       # nPoSePenalty
        + _serialize_int32(0)       # nPoSeRevivedHeight
        + _serialize_int32(0)       # nPoSeBanHeight
        + b"\x00\x00"               # nRevocationReason (uint16)
        + b"\x00" * 32              # confirmedHash
        + b"\x00" * 32              # confirmedHashWithProRegTxHash
        + b"\x00" * 20              # keyIDOwner (uint160)
        + b"\x00" * 48              # pubKeyOperator (legacy CBLSLazyPublicKey)
        + b"\x00" * 20              # keyIDVoting (uint160)
        + b"\x00" * 18              # MnNetInfo: CService V1 (16-byte addr + 2-byte port)
        + _compact_size(0)          # scriptPayout (empty CScript)
        + _compact_size(0)          # scriptOperatorPayout
        + b"\x00" * 20              # platformNodeID (uint160)
        + b"\x00\x00"               # platformP2PPort
        + b"\x00\x00"               # platformHTTPPort
    )

    # CDeterministicMN (src/evo/deterministicmns.h) at PROTOCOL_VERSION:
    #   proTxHash + VARINT(internalId) + collateralOutpoint + nOperatorReward
    #   + pdmnState (shared_ptr inlines the pointed-to object) + nType (uint16, only present
    #   when stream version >= DMN_TYPE_PROTO_VERSION which holds for our PROTOCOL_VERSION prefix).
    minimal_deterministic_mn = (
        b"\x00" * 32                       # proTxHash
        + b"\x00"                          # VARINT(internalId=0)
        + _serialize_outpoint("", 0)       # collateralOutpoint (32 + 4)
        + b"\x00\x00"                      # nOperatorReward (uint16)
        + minimal_dmn_state                # pdmnState
        + b"\x00\x00"                      # nType (MnType, uint16)
    )

    # CSimplifiedMNListEntry (src/evo/simplifiedmns.h) with nVersion=LegacyBLS=1:
    #   PROTOCOL_VERSION >= SMNLE_VERSIONED_PROTO_VERSION so nVersion is always read first.
    #   Because nVersion < BasicBLS, no nType / platform fields are emitted.
    minimal_smn_list_entry = (
        b"\x01\x00"                 # nVersion = LegacyBLS (uint16 LE)
        + b"\x00" * 32              # proRegTxHash
        + b"\x00" * 32              # confirmedHash
        + b"\x00" * 18              # MnNetInfo: V1 CService
        + b"\x00" * 48              # pubKeyOperator (legacy)
        + b"\x00" * 20              # keyIDVoting
        + b"\x00"                   # isValid = false
    )

    # CProUpRegTx (src/evo/providertx.h) at nVersion=LegacyBLS=1:
    #   nVersion(2) + proTxHash(32) + nMode(2) + pubKeyOperator(legacy CBLSLazyPublicKey, 48)
    #   + keyIDVoting(20) + scriptPayout(empty CScript -> CompactSize(0)) + inputsHash(32) + vchSig(empty -> CompactSize(0)).
    minimal_proupreg_tx = (
        b"\x01\x00"                 # nVersion = LegacyBLS
        + b"\x00" * 32              # proTxHash
        + b"\x00\x00"               # nMode (uint16)
        + b"\x00" * 48              # pubKeyOperator (legacy)
        + b"\x00" * 20              # keyIDVoting
        + _compact_size(0)          # scriptPayout (empty)
        + b"\x00" * 32              # inputsHash
        + _compact_size(0)          # vchSig (empty)
    )

    # CProUpRevTx (src/evo/providertx.h) at nVersion=LegacyBLS=1:
    #   nVersion(2) + proTxHash(32) + nReason(2) + inputsHash(32) + sig(legacy CBLSSignature, 96).
    minimal_prouprev_tx = (
        b"\x01\x00"                 # nVersion = LegacyBLS
        + b"\x00" * 32              # proTxHash
        + b"\x00\x00"               # nReason (uint16)
        + b"\x00" * 32              # inputsHash
        + b"\x00" * 96              # sig (legacy CBLSSignature)
    )

    # CGetSimplifiedMNListDiff (src/evo/smldiff.h): just baseBlockHash + blockHash.
    minimal_get_smn_list_diff = b"\x00" * 32 + b"\x00" * 32

    # An empty CPartialMerkleTree (src/merkleblock.h):
    #   nTransactions(uint32=0) + vHash(CompactSize(0)) + bytes(CompactSize(0))
    minimal_partial_merkle_tree = (
        _serialize_uint32(0)
        + _compact_size(0)
        + _compact_size(0)
    )

    # CSimplifiedMNListDiff (src/evo/smldiff.h) at PROTOCOL_VERSION >= MNLISTDIFF_CHAINLOCKS_PROTO_VERSION:
    #   nVersion(uint16) + baseBlockHash(32) + blockHash(32) + cbTxMerkleTree(CPartialMerkleTree)
    #   + cbTx(CMutableTransaction) + deletedMNs(CompactSize(0)) + mnList(CompactSize(0))
    #   + deletedQuorums(CompactSize(0)) + newQuorums(CompactSize(0)) + quorumsCLSigs(CompactSize(0)).
    # nVersion is read first because PROTOCOL_VERSION >= MNLISTDIFF_VERSION_ORDER.
    minimal_smn_list_diff = (
        b"\x01\x00"                          # nVersion
        + b"\x00" * 32                       # baseBlockHash
        + b"\x00" * 32                       # blockHash
        + minimal_partial_merkle_tree        # cbTxMerkleTree
        + minimal_tx                         # cbTx (n_version=2, type=0, empty vin/vout)
        + _compact_size(0)                   # deletedMNs
        + _compact_size(0)                   # mnList
        + _compact_size(0)                   # deletedQuorums
        + _compact_size(0)                   # newQuorums
        + _compact_size(0)                   # quorumsCLSigs
    )

    # llmq P2P / message types — reuse the shared serializers so the synthetic
    # form stays in lockstep with what extract_quorum_info() emits from chain data.
    minimal_quorum_data_request = serialize_quorum_data_request(
        llmq_type=1, quorum_hash_hex="00" * 32, pro_tx_hash_hex="", n_data_mask=1
    )
    minimal_get_quorum_rotation_info = serialize_get_quorum_rotation_info(
        block_request_hash_hex="00" * 32, base_block_hashes_hex=[], extra_share=False
    )
    minimal_quorum_snapshot = serialize_quorum_snapshot(
        active_quorum_members=[], skip_list=[], mn_skip_list_mode=0
    )

    # Targets that need synthetic seeds (serialized structs with known formats)
    synthetic_seeds = {
        # CoinJoin messages — minimal valid-ish payloads
        "dash_coinjoin_accept_deserialize": [
            (_serialize_int32(0) + minimal_tx).hex(),  # nDenom + txCollateral
        ],
        "dash_coinjoin_entry_deserialize": [
            minimal_coinjoin_entry.hex(),
        ],
        "dash_coinjoin_queue_deserialize": [
            (
                _serialize_int32(0)
                + b"\x00" * 32
                + _serialize_int64(0)
                + _serialize_bool(False)
                + _var_bytes(b"")
            ).hex(),
        ],
        "dash_coinjoin_status_update_deserialize": [
            (_serialize_int32(0) + _serialize_int32(0) + _serialize_int32(0) + _serialize_int32(0)).hex(),
        ],
        "dash_coinjoin_broadcast_tx_deserialize": [
            minimal_coinjoin_broadcast_tx.hex(),
        ],
        # LLMQ messages
        "dash_final_commitment_deserialize": [
            minimal_final_commitment.hex(),
        ],
        "dash_final_commitment_tx_payload_deserialize": [
            ((1).to_bytes(2, "little") + _serialize_uint32(0) + minimal_final_commitment).hex(),
        ],
        "dash_recovered_sig_deserialize": [
            # CRecoveredSig: llmqType (uint8) + quorumHash (32) + id (32) + msgHash (32) + sig (96)
            "64" + "00" * 32 + "00" * 32 + "00" * 32 + "00" * 96,
        ],
        "dash_sig_ses_ann_deserialize": [
            # CSigSesAnn: VARINT(sessionId=0) + llmqType (uint8) + quorumHash (32) + id (32) + msgHash (32)
            "00" + "64" + "00" * 32 + "00" * 32 + "00" * 32,
        ],
        "dash_sig_share_deserialize": [
            # CSigShare: llmqType (uint8) + quorumHash (32) + quorumMember (uint16 LE)
            #          + id (32) + msgHash (32) + sigShare (96, BLS lazy)
            "64" + "00" * 32 + "0000" + "00" * 32 + "00" * 32 + "00" * 96,
        ],
        # MNAuth
        "dash_mnauth_deserialize": [
            "00" * 32 + "00" * 32 + "00" * 96,  # proRegTxHash + signChallenge + sig
        ],
        # DKG messages
        "dash_dkg_complaint_deserialize": [
            # CDKGComplaint: llmqType + quorumHash + proTxHash + DYNBITSET(badMembers)
            #              + DYNBITSET(complainForMembers) + sig (96 bytes)
            "64" + "00" * 32 + "00" * 32 + "00" + "00" + "00" * 96,
        ],
        "dash_dkg_justification_deserialize": [
            # llmqType (uint8) + quorumHash (32) + proTxHash (32) +
            # CompactSize(0) for contributions vector + CBLSSignature (96)
            ("64" + "00" * 32 + "00" * 32 + "00" + "00" * 96),
        ],
        "dash_dkg_premature_commitment_deserialize": [
            minimal_premature_commitment.hex(),
        ],
        # Sig-share inventory / batched messages
        "dash_sig_shares_inv_deserialize": [
            # VARINT(sessionId=0) + CompactSize(invSize=0) + AUTOBITSET selector=0
            "000000",
        ],
        "dash_batched_sig_shares_deserialize": [
            # VARINT(sessionId=0) + CompactSize(0) for empty sigShares vector
            "0000",
        ],
        # MN HF signal
        "dash_mnhf_tx_deserialize": [
            # versionBit (uint8) + quorumHash (32) + CBLSSignature non-legacy (96)
            ("00" + "00" * 32 + "00" * 96),
        ],
        # Governance
        "dash_governance_vote_deserialize": [
            minimal_governance_vote.hex(),
        ],
        "dash_vote_instance_deserialize": [
            minimal_vote_instance.hex(),
        ],
        "dash_vote_rec_deserialize": [
            minimal_vote_rec.hex(),
        ],
        "dash_governance_vote_file_deserialize": [
            minimal_vote_file.hex(),
        ],
        # BLS IES
        "dash_bls_ies_encrypted_blob_deserialize": [
            minimal_bls_ies_blob.hex(),
        ],
        "dash_bls_ies_multi_recipient_blobs_deserialize": [
            minimal_bls_ies_multi.hex(),
        ],
        # evo/ types — minimal valid serializations matching the C++ layouts above.
        "dash_credit_pool_deserialize": [
            minimal_credit_pool.hex(),
        ],
        "dash_dmn_state_deserialize": [
            minimal_dmn_state.hex(),
        ],
        "dash_deterministic_mn_deserialize": [
            minimal_deterministic_mn.hex(),
        ],
        "dash_smn_list_entry_deserialize": [
            minimal_smn_list_entry.hex(),
        ],
        "dash_proupreg_tx_deserialize": [
            minimal_proupreg_tx.hex(),
        ],
        "dash_prouprev_tx_deserialize": [
            minimal_prouprev_tx.hex(),
        ],
        "dash_get_smn_list_diff_deserialize": [
            minimal_get_smn_list_diff.hex(),
        ],
        "dash_smn_list_diff_deserialize": [
            minimal_smn_list_diff.hex(),
        ],
        # llmq P2P/message types — keep in sync with extract_quorum_info().
        "dash_quorum_data_request_deserialize": [
            minimal_quorum_data_request.hex(),
        ],
        "dash_get_quorum_rotation_info_deserialize": [
            minimal_get_quorum_rotation_info.hex(),
        ],
        "dash_quorum_snapshot_deserialize": [
            minimal_quorum_snapshot.hex(),
        ],
    }

    for target, seeds in synthetic_seeds.items():
        for seed_hex in seeds:
            if save_corpus_input(output_dir, target, seed_hex):
                saved += 1
            # Also save roundtrip variant
            roundtrip_target = target.replace("_deserialize", "_roundtrip")
            if target not in _DESERIALIZE_ONLY_DASH_TARGETS and save_corpus_input(
                output_dir, roundtrip_target, seed_hex
            ):
                saved += 1

    print(f"  Created {saved} synthetic seed inputs")
    return saved


def _run_helper_self_checks():
    """Deterministic checks for new governance/final-commitment helpers."""
    parsed_vote = parse_governance_vote_record(
        "11" * 32 + "-2:1700000000:yes:funding:1",
        "22" * 32,
    )
    assert parsed_vote["masternode_txid"] == "11" * 32
    assert parsed_vote["masternode_n"] == 2
    assert parsed_vote["signal"] == 1
    assert parsed_vote["outcome"] == 1
    vote_bytes = serialize_governance_vote(parsed_vote)
    assert len(vote_bytes) == 32 + 4 + 32 + 4 + 4 + 8 + 1

    vote_instance = serialize_vote_instance(1, 1700000000, 1690000000)
    assert len(vote_instance) == 20
    vote_rec = serialize_vote_rec({1: vote_instance, 2: serialize_vote_instance(2, 1700000100, 1690000000)})
    assert vote_rec.startswith(b"\x02")
    vote_file = serialize_governance_vote_file([vote_bytes])
    assert vote_file[:4] == _serialize_int32(1)

    payload = bytes.fromhex("0100") + _serialize_uint32(42) + b"\xaa\xbb\xcc"
    assert _final_commitment_from_tx_payload(payload.hex()) == b"\xaa\xbb\xcc"

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        create_synthetic_seeds(tmp_path)
        required_targets = [
            "dash_final_commitment_deserialize",
            "dash_final_commitment_roundtrip",
            "dash_governance_vote_deserialize",
            "dash_vote_instance_deserialize",
            "dash_vote_rec_deserialize",
            "dash_governance_vote_file_deserialize",
            "dash_bls_ies_encrypted_blob_deserialize",
            "dash_bls_ies_encrypted_blob_roundtrip",
            "dash_bls_ies_multi_recipient_blobs_deserialize",
            "dash_bls_ies_multi_recipient_blobs_roundtrip",
            "dash_coinjoin_entry_deserialize",
            "dash_coinjoin_entry_roundtrip",
            "dash_dkg_complaint_deserialize",
            "dash_dkg_complaint_roundtrip",
            "dash_dkg_justification_deserialize",
            "dash_dkg_justification_roundtrip",
            "dash_sig_shares_inv_deserialize",
            "dash_sig_shares_inv_roundtrip",
            "dash_batched_sig_shares_deserialize",
            "dash_batched_sig_shares_roundtrip",
            "dash_recovered_sig_deserialize",
            "dash_recovered_sig_roundtrip",
            "dash_sig_ses_ann_deserialize",
            "dash_sig_ses_ann_roundtrip",
            "dash_sig_share_deserialize",
            "dash_sig_share_roundtrip",
            "dash_mnauth_deserialize",
            "dash_mnauth_roundtrip",
            "dash_mnhf_tx_deserialize",
            "dash_credit_pool_deserialize",
            "dash_credit_pool_roundtrip",
            "dash_dmn_state_deserialize",
            "dash_dmn_state_roundtrip",
            "dash_deterministic_mn_deserialize",
            "dash_deterministic_mn_roundtrip",
            "dash_smn_list_entry_deserialize",
            "dash_smn_list_entry_roundtrip",
            "dash_proupreg_tx_deserialize",
            "dash_proupreg_tx_roundtrip",
            "dash_prouprev_tx_deserialize",
            "dash_prouprev_tx_roundtrip",
            "dash_get_smn_list_diff_deserialize",
            "dash_get_smn_list_diff_roundtrip",
            "dash_smn_list_diff_deserialize",
            "dash_smn_list_diff_roundtrip",
            "dash_quorum_data_request_deserialize",
            "dash_quorum_data_request_roundtrip",
            "dash_get_quorum_rotation_info_deserialize",
            "dash_get_quorum_rotation_info_roundtrip",
            "dash_quorum_snapshot_deserialize",
            "dash_quorum_snapshot_roundtrip",
        ]
        missing = [target for target in required_targets if not any((tmp_path / target).iterdir())]
        assert not missing, f"missing synthetic seeds for: {', '.join(missing)}"

        # Assert the LLMQ seed sizes match the C++ serialization layouts so the
        # synthetic seeds aren't silently truncated again (regression guard).
        def _seed_bytes(target):
            files = list((tmp_path / target).iterdir())
            assert len(files) == 1, f"{target}: expected one synthetic seed, got {len(files)}"
            return files[0].read_bytes()

        # The Dash deserialize/roundtrip wrappers prepend a 4-byte stream version.
        prefix = len(_stream_version_prefix())
        # CRecoveredSig: 1 + 32 + 32 + 32 + 96 = 193
        assert len(_seed_bytes("dash_recovered_sig_deserialize")) == prefix + 193
        # CSigSesAnn (VARINT(0)=1 byte): 1 + 1 + 32 + 32 + 32 = 98
        assert len(_seed_bytes("dash_sig_ses_ann_deserialize")) == prefix + 98
        # CSigShare: 1 + 32 + 2 + 32 + 32 + 96 = 195
        assert len(_seed_bytes("dash_sig_share_deserialize")) == prefix + 195
        # CDKGComplaint with empty bitsets: 1 + 32 + 32 + 1 + 1 + 96 = 163
        assert len(_seed_bytes("dash_dkg_complaint_deserialize")) == prefix + 163
        # CCreditPool: int64*3 + CompactSize(0) = 8*3 + 1 = 25
        assert len(_seed_bytes("dash_credit_pool_deserialize")) == prefix + 25
        # CDeterministicMNState (LegacyBLS layout): see minimal_dmn_state above.
        # 4*7 + 2 + 32 + 32 + 20 + 48 + 20 + 18 + 1 + 1 + 20 + 2 + 2 = 226
        assert len(_seed_bytes("dash_dmn_state_deserialize")) == prefix + 226
        # CDeterministicMN: 32 + 1 + 36 + 2 + 226 + 2 = 299
        assert len(_seed_bytes("dash_deterministic_mn_deserialize")) == prefix + 299
        # CSimplifiedMNListEntry (LegacyBLS): 2 + 32 + 32 + 18 + 48 + 20 + 1 = 153
        assert len(_seed_bytes("dash_smn_list_entry_deserialize")) == prefix + 153
        # CProUpRegTx (LegacyBLS): 2 + 32 + 2 + 48 + 20 + 1 + 32 + 1 = 138
        assert len(_seed_bytes("dash_proupreg_tx_deserialize")) == prefix + 138
        # CProUpRevTx (LegacyBLS): 2 + 32 + 2 + 32 + 96 = 164
        assert len(_seed_bytes("dash_prouprev_tx_deserialize")) == prefix + 164
        # CGetSimplifiedMNListDiff: 32 + 32 = 64
        assert len(_seed_bytes("dash_get_smn_list_diff_deserialize")) == prefix + 64
        # CSimplifiedMNListDiff at PROTOCOL_VERSION (>= MNLISTDIFF_CHAINLOCKS_PROTO_VERSION):
        # nVersion(2) + baseBlockHash(32) + blockHash(32)
        # + cbTxMerkleTree(uint32+CompactSize(0)+CompactSize(0)=6)
        # + cbTx(n32bitVersion(4)+CompactSize(0)+CompactSize(0)+nLockTime(4)=10)
        # + 5*CompactSize(0) (deletedMNs/mnList/deletedQuorums/newQuorums/quorumsCLSigs) = 5
        # = 2 + 64 + 6 + 10 + 5 = 87
        assert len(_seed_bytes("dash_smn_list_diff_deserialize")) == prefix + 87
        # CQuorumDataRequest: llmqType(1) + quorumHash(32) + nDataMask(2) + proTxHash(32) + nError(1) = 68
        assert len(_seed_bytes("dash_quorum_data_request_deserialize")) == prefix + 68
        # CGetQuorumRotationInfo (empty baseBlockHashes): CompactSize(0)+blockRequestHash(32)+bool(1) = 34
        assert len(_seed_bytes("dash_get_quorum_rotation_info_deserialize")) == prefix + 34
        # CQuorumSnapshot (empty active members + empty skip list):
        # mnSkipListMode(int32=4) + CompactSize(0) for active bitset + CompactSize(0) for skip list = 6
        assert len(_seed_bytes("dash_quorum_snapshot_deserialize")) == prefix + 6

    # --stream-version override path: setting the override must not require
    # src/version.h, and _stream_version_prefix must round-trip the value.
    global _STREAM_VERSION_OVERRIDE, _STREAM_VERSION_CACHE
    saved_override, saved_cache = _STREAM_VERSION_OVERRIDE, _STREAM_VERSION_CACHE
    try:
        _STREAM_VERSION_OVERRIDE = 0x12345678
        _STREAM_VERSION_CACHE = None
        assert _stream_version_prefix() == b"\x78\x56\x34\x12"
    finally:
        _STREAM_VERSION_OVERRIDE = saved_override
        _STREAM_VERSION_CACHE = saved_cache


def main():
    parser = argparse.ArgumentParser(
        description="Extract seed corpus from a running Dash node for fuzz testing"
    )
    # --output-dir is required for normal/synthetic-only runs but not for --self-check,
    # so the flag is enforced post-parse rather than via argparse's required=True.
    parser.add_argument(
        "--output-dir", "-o",
        help="Output directory for corpus files (required unless --self-check)"
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
    parser.add_argument(
        "--self-check",
        action="store_true",
        help=(
            "Run deterministic helper self-checks (governance/final-commitment "
            "serializers and the synthetic seed generator) and exit. Does not "
            "require --output-dir."
        ),
    )
    parser.add_argument(
        "--stream-version",
        type=int,
        default=None,
        help=(
            "Stream version (4-byte LE prefix) to use for harnesses that consume one. "
            "Overrides DASH_FUZZ_STREAM_VERSION and the src/version.h fallback. "
            "Useful when running outside an in-tree source checkout."
        ),
    )
    args = parser.parse_args()

    if args.stream_version is not None:
        global _STREAM_VERSION_OVERRIDE
        _STREAM_VERSION_OVERRIDE = args.stream_version

    if args.self_check:
        _run_helper_self_checks()
        print("seed_corpus_from_chain.py: self-check passed")
        return

    if not args.output_dir:
        parser.error("--output-dir/-o is required unless --self-check is passed")

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
