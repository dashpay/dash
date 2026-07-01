# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

GroveDB is a hierarchical authenticated data structure database - essentially a "grove" (tree of trees) built on Merkle AVL trees. It provides efficient secondary index queries, cryptographic proofs, and is optimized for blockchain applications like Dash Platform.

## Development Commands

```bash
# Build the project
cargo build

# Run all tests
cargo test

# Run a specific test
cargo test test_name

# Run tests for a specific crate
cargo test -p grovedb
cargo test -p merk
cargo test -p storage

# Run with verbose output
cargo test -- --nocapture

# Build with all features
cargo build --features full,estimated_costs

# Run benchmarks
cargo bench

# Format code
cargo fmt

# Run linter
cargo clippy -- -D warnings

# Build documentation
cargo doc --open

# Run tests with specific features
cargo test --features full,verify
```

## Deep Architecture Understanding

### System Layers

1. **GroveDB Core** (`grovedb/src/`)
   - Orchestrates multiple Merk trees into a hierarchical structure
   - Each tree element can contain another Merk tree, creating a "grove"
   - Manages references between elements across trees
   - Handles batch operations atomically across multiple subtrees
   - Generates composite proofs spanning multiple trees

2. **Merk Layer** (`merk/src/`)
   - Merkle AVL tree implementation with self-balancing
   - Uses a unique node structure where intermediary nodes store key-value pairs
   - Supports lazy loading via the Link system (Reference/Modified/Uncommitted/Loaded)
   - Implements chunk-based restoration for large trees
   - Cost-aware operations with predefined costs for specialized nodes

3. **Storage Layer** (`storage/src/`)
   - Abstracts RocksDB with prefixed storage for subtree isolation
   - Uses Blake3 hashing to generate 32-byte prefixes from paths
   - Supports optimistic transactions via OptimisticTransactionDB
   - Four storage types: main data, auxiliary, roots, metadata
   - Batch operations minimize disk I/O

### Critical Design Patterns

#### Element System
```rust
// 8 element types with specific use cases:
Element::Item           // Basic key-value storage
Element::Reference      // Links between elements (7 reference types)
Element::Tree          // Container for subtrees
Element::SumItem       // Contributes to parent sum
Element::SumTree       // Maintains sum of descendants
Element::BigSumTree    // 256-bit sums
Element::CountTree     // Element counting
Element::CountSumTree  // Combined functionality
```

#### Reference Types
- **AbsolutePathReference**: Direct path from root
- **UpstreamRootHeightReference**: Navigate up N levels, then follow path
- **UpstreamFromElementHeightReference**: Relative to current element
- **CousinReference**: Same tree level, different branch
- **SiblingReference**: Same parent tree
- **UpstreamRootHeightWithParentPathAddition**: Complex navigation
- **UtilityReference**: System-level references

#### Cost Tracking
Every operation accumulates costs:
- `seek_count`: Database seeks
- `storage_loaded_bytes`: Data read from disk
- `storage_cost`: Added/replaced/removed bytes
- `hash_node_calls`: Cryptographic operations

### Key Implementation Details

#### Proof System
- Layer-by-layer proof generation from root to target
- Stack-based proof verification using virtual machine operations
- Supports absence proofs for non-existing keys
- Optimizes proof size by excluding unnecessary data

#### Query System (PathQuery)
```rust
PathQuery {
    path: Vec<Vec<u8>>,     // Starting location
    query: SizedQuery {
        query: Query {
            items: Vec<QueryItem>,  // What to select
            default_subquery_branch,
            conditional_subquery_branches,
            left_to_right: bool,
            add_parent_tree_on_subquery: bool,  // v2: Include parent tree in results
        },
        limit: Option<u16>,
        offset: Option<u16>,
    }
}
```

**New in v2**: The `add_parent_tree_on_subquery` flag allows including parent tree elements (like CountTree or SumTree) in query results when performing subqueries. This is useful when you need both aggregate values and individual elements.

#### Batch Operations
- Two-phase processing: validation then application
- TreeCache for deferred root hash propagation
- Atomic operations across multiple subtrees
- Support for transient operations

### Important Files and Their Roles

#### Core GroveDB
- `grovedb/src/grove_db.rs`: Main struct and public API
- `grovedb/src/operations/insert/mod.rs`: Insert logic with element validation
- `grovedb/src/operations/delete/mod.rs`: Delete operations including delete_up_tree
- `grovedb/src/operations/proof/generate.rs`: Multi-tree proof generation
- `grovedb/src/batch/mod.rs`: Batch operation processing
- `grovedb/src/reference_path/mod.rs`: Reference resolution logic

#### Merk Implementation
- `merk/src/tree/mod.rs`: AVL tree core with balancing
- `merk/src/tree/walk/mod.rs`: Walker pattern for lazy loading
- `merk/src/tree/ops.rs`: Tree operations (put, delete)
- `merk/src/proofs/query/mod.rs`: Query execution and proof generation
- `merk/src/proofs/encoding.rs`: Proof serialization
- `merk/src/owner.rs`: Reference counting wrapper

#### Storage Abstraction
- `storage/src/rocksdb_storage/storage.rs`: RocksDB implementation
- `storage/src/rocksdb_storage/storage_context.rs`: Prefixed contexts
- `storage/src/batch.rs`: Batch operation accumulation

### Testing Philosophy

1. **Proof Verification**: Every operation that modifies state must be testable with proofs
2. **Cost Accuracy**: Tests verify cost calculations match actual operations
3. **Reference Integrity**: Tests ensure references don't create cycles
4. **Version Compatibility**: Tests run against multiple grove versions
5. **Batch Atomicity**: Tests verify all-or-nothing batch behavior

### Common Development Patterns

#### Adding New Features
1. Check grove version compatibility first
2. Implement cost calculation alongside functionality
3. Ensure proof generation works correctly
4. Add batch operation support
5. Write comprehensive tests including edge cases

#### Error Handling
```rust
// Use cost_return_on_error for early returns with cost accumulation
cost_return_on_error!(&mut cost, result);

// Wrap errors with context
.map_err(|e| Error::CorruptedData(format!("context: {}", e)))?;
```

#### Performance Considerations
1. Use batch operations for multiple changes
2. Leverage MerkCache for frequently accessed trees
3. Minimize tree opens by using persistent contexts
4. Consider cost limits for expensive operations
5. Use lazy loading to avoid loading unnecessary data

### Debugging Tips

1. **Visualizer**: Use `db.start_visualizer(port)` for web-based debugging
2. **Cost Analysis**: Log operation costs to identify expensive operations
3. **Proof Verification**: Test proofs independently to isolate issues
4. **Reference Tracing**: Follow references manually to debug resolution
5. **Version Checks**: Ensure correct version is used throughout

### Security Considerations

1. **Proof Integrity**: Never trust unverified proofs
2. **Reference Limits**: Always enforce hop limits to prevent DoS
3. **Cost Limits**: Set reasonable limits to prevent resource exhaustion
4. **Input Validation**: Validate all paths and keys before operations
5. **Transaction Safety**: Use transactions for multi-step operations

## Workspace Structure

```
grovedb/
├── grovedb/           # Main database implementation
├── merk/              # Merkle AVL tree engine
├── storage/           # Storage abstraction layer
├── costs/             # Cost tracking utilities
├── path/              # Path manipulation utilities
├── grovedb-version/   # Version management
├── grovedb-epoch-based-storage-flags/  # Epoch storage features
├── visualize/         # Debug visualization
├── node-grove/        # Node.js bindings
└── docs/             # Detailed documentation
    └── crates/       # Per-crate documentation
```

## Key Algorithms

### AVL Tree Balancing
- Balance factor = right_height - left_height
- Must maintain factor ∈ {-1, 0, 1}
- Single/double rotations for rebalancing
- O(log n) operations guaranteed

### Prefix Generation
- Convert path segments to bytes
- Apply Blake3 hash for 32-byte prefix
- Ensures subtree isolation in storage

### Proof Generation
- Depth-first traversal collecting nodes
- Stack-based operation encoding
- Minimal proof size optimization

When working on GroveDB, always consider the hierarchical nature of the system and how changes propagate through the tree structure. Every operation must maintain cryptographic integrity while being cost-efficient.