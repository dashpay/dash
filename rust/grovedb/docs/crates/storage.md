# Storage - Abstraction Layer for Persistent Storage

## Overview

The Storage crate provides a clean abstraction over different storage backends, with a primary implementation using RocksDB. It enables GroveDB to efficiently manage multiple Merkle trees within a single database instance while providing transaction support, cost tracking, and performance optimizations.

## Architecture

### Core Traits

#### Storage Trait
```rust
pub trait Storage<'db> {
    type Transaction: StorageTransaction<'db>;
    type BatchTransaction: StorageBatchTransaction<'db>;
    type Immediate: StorageImmediate<'db>;
    
    fn start_transaction(&'db self) -> Self::Transaction;
    fn commit_transaction(&self, transaction: Self::Transaction) -> Result<(), Error>;
    fn rollback_transaction(&self, transaction: Self::Transaction) -> Result<(), Error>;
    fn create_checkpoint<P: AsRef<Path>>(&self, path: P) -> Result<(), Error>;
    fn flush(&self) -> Result<(), Error>;
}
```

The `Storage` trait defines the top-level interface for storage backends, managing transactions and database lifecycle operations.

#### StorageContext Trait
```rust
pub trait StorageContext<'db> {
    type Batch: StorageBatch<'db>;
    type RawIterator: StorageRawIterator;
    
    fn put<K: AsRef<[u8]>, V: AsRef<[u8]>>(&self, key: K, value: V, ...) -> CostResult<(), Error>;
    fn get<K: AsRef<[u8]>>(&self, key: K, ...) -> CostResult<Option<Vec<u8>>, Error>;
    fn delete<K: AsRef<[u8]>>(&self, key: K, ...) -> CostResult<(), Error>;
    fn new_batch(&self) -> Self::Batch;
    fn raw_iter(&self) -> Self::RawIterator;
}
```

`StorageContext` provides operations within a specific subtree context, supporting CRUD operations and iteration.

### RocksDB Implementation

#### Database Configuration
```rust
pub struct RocksDbStorage {
    db: Arc<OptimisticTransactionDB>,
    column_families: HashMap<String, ColumnFamily>,
}
```

**Key Features**:
- **OptimisticTransactionDB**: ACID transactions with optimistic concurrency control
- **Column Families**: Separates different data types
  - Default: Main data storage
  - `aux`: Auxiliary data
  - `roots`: Subtree roots
  - `meta`: Metadata

**Optimized Settings**:
- Memory-mapped I/O for reads and writes
- Parallelism based on CPU cores
- Atomic flush across column families
- Auto-creation of missing column families

### Prefixed Storage Design

#### Purpose
The prefixed storage design enables efficient subtree isolation:
- Multiple Merk trees share the same RocksDB instance
- Each subtree has a unique 32-byte prefix
- Prevents key collisions between subtrees
- Enables efficient range queries within subtrees

#### Implementation
```rust
fn make_prefixed_key(prefix: &SubtreePrefix, key: &[u8]) -> Vec<u8> {
    let mut prefixed_key = Vec::with_capacity(prefix.len() + key.len());
    prefixed_key.extend_from_slice(prefix);
    prefixed_key.extend_from_slice(key);
    prefixed_key
}
```

**Prefix Generation**:
1. Convert subtree path to bytes
2. Apply Blake3 hash to create 32-byte prefix
3. Prepend prefix to all keys in that subtree

### Transaction System

#### Transaction Types

1. **BatchTransactionalStorageContext**
   - Defers operations into a batch
   - Applied atomically on commit
   - Used for normal operations

2. **ImmediateStorageContext**
   - Applies operations directly
   - Used for replication scenarios
   - Bypasses batching

#### Transaction Flow
```rust
// Start transaction
let transaction = storage.start_transaction();

// Create contexts for subtrees
let context1 = transaction.create_context(path1);
let context2 = transaction.create_context(path2);

// Perform operations
context1.put(key, value)?;
context2.delete(key)?;

// Commit atomically
storage.commit_transaction(transaction)?;
```

### Batch Operations

#### StorageBatch
High-level batch for accumulating operations:
```rust
pub struct StorageBatch {
    operations: RefCell<BTreeMap<Vec<u8>, BatchOperation>>,
    pending_costs: RefCell<OperationCost>,
}
```

**Features**:
- Interior mutability with `RefCell`
- Operations stored in `BTreeMap` for ordering
- Automatic deduplication (latest operation wins)
- Cost accumulation before commit

#### WriteBatchWithTransaction
RocksDB-level batch for atomic writes:
- Native RocksDB batch implementation
- Converted from `StorageBatch` during commit
- Minimizes disk I/O by grouping operations

### Cost Tracking

#### Cost Context System
```rust
pub struct CostContext {
    value: T,
    cost: OperationCost,
}
```

**Tracked Metrics**:
- `seek_count`: Database seeks performed
- `storage_loaded_bytes`: Bytes read from storage
- `storage_cost`: Added/removed bytes
- `hash_node_calls`: Blake3 hashing operations

#### Cost Calculation
- Key-value storage includes key length, value length, metadata
- Children sizes tracked for tree nodes
- Removal costs based on actual data removed
- Special handling for sum trees and feature types

### Storage Types

The storage layer supports four distinct storage types:

1. **Main Storage**: Primary key-value data
2. **Auxiliary Storage**: Secondary data and indexes
3. **Roots Storage**: Subtree root hashes
4. **Metadata Storage**: System metadata

Each type can be accessed through the storage context:
```rust
context.put_aux(key, value, grove_version)?;
context.get_meta(key)?;
context.delete_root(key, grove_version)?;
```

## Performance Optimizations

### Memory-Mapped I/O
- Reduces system calls for reads/writes
- Improves throughput for large datasets
- Configurable through RocksDB options

### Batch Processing
- Groups multiple operations into single disk write
- Reduces write amplification
- Improves transaction throughput

### Prefix Iteration
```rust
let prefix_iter = context.raw_iter();
prefix_iter.seek(prefix);
while prefix_iter.valid() && prefix_iter.key().starts_with(prefix) {
    // Process key-value pair
    prefix_iter.next();
}
```

Enables efficient subtree traversal without scanning entire database.

### Cost-Aware Operations
- Skip unnecessary work based on cost limits
- Early termination when cost exceeded
- Efficient resource usage tracking

## Error Handling

### Error Types
```rust
pub enum StorageError {
    RocksDBError(String),
    CorruptedData(String),
    PathError(String),
    CostError(String),
}
```

### Error Propagation
- `CostResult<T, Error>` wraps results with costs
- `cost_return_on_error!` macros for early returns
- Cost accumulation even on error paths
- Graceful degradation where possible

## Usage Examples

### Basic Operations
```rust
// Create storage
let storage = RocksDbStorage::new(path)?;

// Start transaction
let tx = storage.start_transaction();
let context = tx.create_context(subtree_path);

// Perform operations
context.put(b"key", b"value", grove_version)?;
let value = context.get(b"key")?;
context.delete(b"key", grove_version)?;

// Commit transaction
storage.commit_transaction(tx)?;
```

### Batch Operations
```rust
// Create batch
let batch = context.new_batch();

// Add operations
batch.put(b"key1", b"value1", grove_version)?;
batch.put(b"key2", b"value2", grove_version)?;
batch.delete(b"key3", grove_version)?;

// Apply batch
context.apply_batch(batch)?;
```

### Iteration
```rust
// Create iterator
let mut iter = context.raw_iter();

// Seek to prefix
iter.seek(prefix);

// Iterate through keys
while iter.valid() {
    let key = iter.key();
    let value = iter.value();
    // Process key-value pair
    iter.next();
}
```

## Design Philosophy

The storage layer is designed with several principles:

1. **Abstraction**: Clean separation between storage interface and implementation
2. **Performance**: Optimized for blockchain workloads with batch operations
3. **Safety**: Transaction support ensures data consistency
4. **Flexibility**: Support for multiple storage backends
5. **Cost Awareness**: Detailed resource tracking for all operations

## Integration with GroveDB

The storage layer integrates tightly with GroveDB:
- Each Merk tree gets its own storage context with unique prefix
- Transactions span multiple trees for atomic updates
- Cost tracking flows from storage through Merk to GroveDB
- Prefixed design enables efficient subtree operations

## Future Enhancements

- Additional storage backend implementations
- Concurrent transaction support
- Advanced caching strategies
- Compression options for stored data
- Enhanced monitoring and metrics