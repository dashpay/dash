# merk

*High-performance Merkle key/value store*

![CI](https://github.com/nomic-io/merk/actions/workflows/ci.yml/badge.svg)
[![codecov](https://codecov.io/gh/nomic-io/merk/branch/develop/graph/badge.svg?token=TTUTSt2iLz)](https://codecov.io/gh/nomic-io/merk)
[![Crate](https://img.shields.io/crates/v/merk.svg)](https://crates.io/crates/merk)
[![API](https://docs.rs/merk/badge.svg)](https://docs.rs/merk)

Merk is a crypto key/value store - more specifically, it's a Merkle AVL tree built on top of RocksDB (Facebook's fork of LevelDB).

Its priorities are performance and reliability. While Merk was designed to be the state database for blockchains, it can also be used anywhere an auditable key/value store is needed.

### FEATURES:
- **Fast reads/writes** - Reads have no overhead compared to a normal RocksDB store, and writes are optimized for batch operations (e.g. blocks in a blockchain).
- **Fast proof generation** - Since Merk implements an AVL tree rather than a trie, it is very efficient to create and verify proofs for ranges of keys.
- **Concurrency** - Unlike most other Merkle stores, all operations utilize all available cores - giving huge performance gains and allowing nodes to scale along with Moore's Law.
- **Replication** - The tree is optimized to efficiently build proofs of large chunks, allowing for nodes to download the entire state (e.g. "state syncing").
- **Checkpointing** - Merk can create checkpoints on disk (an immutable view of the entire store at a certain point in time) without blocking, so there are no delays in availability or liveness.
- **Web-friendly** - Being written in Rust means it is easy to run the proof-verification code in browsers with WebAssembly, allowing for light-clients that can verify data for themselves.
- **Fits any Profile** - Performant on RAM-constrained Raspberry Pi's and beefy validator rigs alike.

## Usage

**Install:**
```
cargo add merk
```

**Example:**
```rust
extern crate merk;
use merk::*;

// load or create a Merk store at the given path
let mut merk = Merk::open("./merk.db").unwrap();

// apply some operations
let batch = [
    (b"key", Op::Put(b"value")),
    (b"key2", Op::Put(b"value2")),
    (b"key3", Op::Put(b"value3")),
    (b"key4", Op::Delete)
];
merk.apply(&batch).unwrap();
```

## Status

Merk is being used in the [Nomic](https://github.com/nomic-io/nomic) Bitcoin Sidechain.

The codebase has not been audited but has been throroughly tested and proves to be stable.

## Benchmarks

Benchmarks are measured on a 1M node tree, each node having a key length of 16 bytes and value length of 40 bytes. All tests are single-threaded (not counting RocksDB background threads).

You can test these yourself by running `cargo bench`.

### 2017 Macbook Pro

*(Using 1 Merk thread and 4 RocksDB compaction threads)*

**Pruned (no state kept in memory)**

*RAM usage:* ~20MB average, ~26MB max

| Test | Ops per second |
| -------- | ------ |
| Random inserts | 23,000 |
| Random updates | 32,000 |
| Random deletes | 26,000 |
| Random reads | 210,000 |
| Random proof generation | 133,000 |

**Cached (all state kept in memory)**

*RAM usage:* ~400MB average, ~1.1GB max

| Test | Ops per second |
| -------- | ------ |
| Random inserts | 58,000 |
| Random updates | 81,000 |
| Random deletes | 72,000 |
| Random reads | 1,565,000 |
| Random proof generation | 311,000 |

### i9-9900K Desktop

*(Using 1 Merk thread and 16 RocksDB compaction threads)*

**Pruned (no state kept in memory)**

*RAM usage:* ~20MB average, ~26MB max

| Test | Ops per second |
| -------- | ------ |
| Random inserts | 40,000 |
| Random updates | 55,000 |
| Random deletes | 45,000 |
| Random reads | 383,000 |
| Random proof generation | 249,000 |

**Cached (all state kept in memory)**

*RAM usage:* ~400MB average, ~1.1GB max

| Test | Ops per second |
| -------- | ------ |
| Random inserts | 93,000 |
| Random updates | 123,000 |
| Random deletes | 111,000 |
| Random reads | 2,370,000 |
| Random proof generation | 497,000 |

## Algorithm Details

The algorithms are based on AVL, but optimized for batches of operations and random fetches from the backing store. Read about the algorithms here: https://github.com/nomic-io/merk/blob/develop/docs/algorithms.md
