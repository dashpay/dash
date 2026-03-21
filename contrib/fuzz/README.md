# Dash Core Fuzz Testing Tools

This directory contains tools for continuous fuzz testing of Dash Core.

## Overview

Dash Core inherits ~100 fuzz targets from Bitcoin Core and adds Dash-specific
targets for:
- Special transaction serialization (ProTx, CoinJoin, Asset Lock/Unlock, etc.)
- BLS operations and IES encryption
- LLMQ/DKG message handling
- Governance object validation
- Masternode list management

Some Dash-specific fuzz targets are planned/in-progress. Corpus tooling
pre-generates synthetic seeds for those target names so coverage is ready when
the targets are added.

## Tools

### `continuous_fuzz_daemon.sh`

A daemon script that continuously cycles through all fuzz targets with persistent
corpus storage and crash detection.

```bash
# Run all targets, 10 minutes each, indefinitely
./continuous_fuzz_daemon.sh --fuzz-bin /path/to/fuzz --time-per-target 600

# Run specific targets only
./continuous_fuzz_daemon.sh --targets bls_operations,bls_ies --time-per-target 3600

# Single cycle (good for cron)
./continuous_fuzz_daemon.sh --single-cycle --time-per-target 300

# Dry run — list targets
./continuous_fuzz_daemon.sh --dry-run
```

**Output directories:**
- `~/fuzz_corpus/<target>/` — persistent corpus per target
- `~/fuzz_crashes/<target>/` — crash artifacts (crash-*, timeout-*, oom-*)
- `~/fuzz_logs/` — per-target logs and daemon log

### `seed_corpus_from_chain.py`

Extracts real-world data from a running Dash node into fuzzer-consumable corpus
files. Connects via `dash-cli` RPC.

```bash
# Extract from a running node
./seed_corpus_from_chain.py -o /path/to/corpus --blocks 500

# Generate only synthetic seeds (no running node required)
./seed_corpus_from_chain.py -o /path/to/corpus --synthetic-only
```

**What it extracts:**
- Serialized blocks and block headers
- Special transactions (ProRegTx, ProUpServTx, CoinJoin, Asset Lock, etc.)
- Governance objects and votes
- Masternode list entries
- Quorum commitment data

## CI Integration

The `test-fuzz.yml` workflow runs fuzz regression tests on every PR:

1. Builds fuzz targets with sanitizers (ASan + UBSan + libFuzzer)
2. Downloads seed corpus from `bitcoin-core/qa-assets` + synthetic Dash seeds
3. Replays all corpus inputs against every fuzz target
4. Reports failures as CI errors

This catches regressions in seconds — any code change that causes a previously-
working input to crash will be caught.

## Building Fuzz Targets

```bash
# Configure with fuzzing + sanitizers
./configure --enable-fuzz --with-sanitizers=fuzzer,address,undefined \
    CC='clang -ftrivial-auto-var-init=pattern' \
    CXX='clang++ -ftrivial-auto-var-init=pattern'

# Build
make -j$(nproc)

# The fuzz binary is at src/test/fuzz/fuzz
# Select target with FUZZ=<target_name>
FUZZ=bls_operations ./src/test/fuzz/fuzz corpus_dir/
```

## Contributing Corpus Inputs

Found an interesting input? Add it to the appropriate corpus directory:

```bash
# The filename should be the sha256 of the content (for dedup)
sha256sum input_file
cp input_file fuzz_corpus/<target_name>/<sha256_prefix>
```

Crash-reproducing inputs are especially valuable — they become regression tests.
