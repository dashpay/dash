# Auxiliary Crates

## grovedb-path - Efficient Path Navigation

### Overview
The Path crate provides zero-copy path manipulation utilities for navigating GroveDB's hierarchical structure. It's designed to minimize allocations while providing ergonomic APIs for path operations.

### Core Components

#### SubtreePath
A non-owning view into a path:
```rust
pub struct SubtreePath<'a, B> {
    inner: SubtreePathInner<'a, B>,
}

enum SubtreePathInner<'a, B> {
    None,
    Iterator(Iter<'a, B>),
    Builder(&'a SubtreePathBuilder<B>),
}
```

**Features**:
- Cheap to clone (just copies references)
- Supports iteration over path segments
- Can be created from various sources

#### SubtreePathBuilder
Owned path representation:
```rust
pub struct SubtreePathBuilder<B = Vec<u8>> {
    pub(crate) segments: Vec<B>,
}
```

**Operations**:
- `push()`: Add segment to path
- `parent()`: Get parent path
- `derive_parent()`: Create new parent path
- `to_path()`: Convert to SubtreePath

### Usage Examples
```rust
// Create path from array
let path = SubtreePath::from(&[b"users", b"alice", b"documents"]);

// Build path dynamically
let mut builder = SubtreePathBuilder::new();
builder.push(b"users".to_vec());
builder.push(b"alice".to_vec());

// Iterate segments
for segment in path.iter() {
    println!("Segment: {:?}", segment);
}

// Get parent
let parent = path.derive_parent()?;
```

### Design Benefits
- Zero allocations for common operations
- Flexible input types (arrays, vectors, builders)
- Efficient parent/child navigation
- Display formatting for debugging

---

## grovedb-version - Protocol Version Management

### Overview
Manages versioning across GroveDB to ensure compatibility and enable protocol upgrades. Uses fine-grained version tracking for individual features.

### Version Structure
```rust
pub struct GroveVersion {
    pub protocol_version: u32,
    pub grovedb_versions: GroveDBVersions,
    pub merk_versions: MerkVersions,
}

pub struct GroveDBVersions {
    pub apply_batch: GroveDBApplyBatchVersions,
    pub operations: GroveDBOperationsVersions,
    pub element: GroveDBElementMethodVersions,
    // ... more categories
}
```

### Version Checking
```rust
// Compile-time version check
check_grovedb_v0_with_cost!(
    "insert",
    grove_version.grovedb_versions.operations.insert.insert
);

// Runtime version check
match grove_version.protocol_version {
    1 => handle_v1(),
    2 => handle_v2(),
    _ => return Err(Error::UnsupportedVersion),
}
```

### Key Features
- Hierarchical version organization
- Compile-time and runtime checks
- Smooth upgrade paths
- Feature-specific versioning
- Cost-aware version checks

---

## grovedb-epoch-based-storage-flags - Epoch-Based Storage Management

### Overview
Tracks storage allocation across different epochs with ownership information. Essential for managing storage costs in multi-tenant or time-based systems.

### Storage Flag Types
```rust
pub enum StorageFlags {
    SingleEpoch(StorageRemovedBytes),
    MultiEpoch(MultiEpochStorageFlags),
    SingleEpochOwned(SingleEpochOwned),
    MultiEpochOwned(MultiEpochOwned),
}
```

### Epoch Tracking
```rust
pub struct StorageRemovedBytes {
    pub added_bytes: u64,
    pub total_removed_bytes: u64,
    pub removed_key_bytes: u64,
    pub removed_value_bytes: u64,
}

pub struct MultiEpochStorageFlags {
    pub epochs: BTreeMap<u64, StorageRemovedBytes>,
}
```

### Key Operations
- **Add Storage**: Track bytes added in specific epoch
- **Remove Storage**: LIFO removal with epoch awareness
- **Merge Flags**: Combine storage from multiple sources
- **Owner Management**: Associate storage with owner IDs

### Usage Example
```rust
// Create storage flags for epoch 5
let mut flags = StorageFlags::SingleEpoch(StorageRemovedBytes {
    added_bytes: 1024,
    total_removed_bytes: 0,
    removed_key_bytes: 0,
    removed_value_bytes: 0,
});

// Remove some bytes
flags.remove_bytes(512, &epoch_info)?;

// Merge with other flags
flags.merge(other_flags, merge_strategy)?;
```

---

## grovedb-visualize - Debug Visualization

### Overview
Provides human-friendly visualization of GroveDB data structures for debugging and development. Intelligently formats byte arrays and tree structures.

### Core Components

#### Visualize Trait
```rust
pub trait Visualize {
    fn visualize<W: Write>(&self, drawer: Drawer<W>) -> Result<Drawer<W>>;
}
```

#### Drawer
Manages indentation and formatting:
```rust
pub struct Drawer<W: Write> {
    content: W,
    level: usize,
    indent: usize,
}
```

### Formatting Features
- Tree structure visualization with indentation
- Intelligent byte array display:
  - ASCII for printable characters
  - Hex for binary data
  - Mixed mode for partial ASCII
- Customizable indentation levels

### Usage Example
```rust
use grovedb_visualize::{Visualize, Drawer};

// Visualize a tree structure
let drawer = Drawer::new(std::io::stdout());
tree.visualize(drawer)?;

// Output:
// users
// ├── alice
// │   ├── balance: 100
// │   └── documents
// │       ├── doc1: [48 65 6c 6c 6f]
// │       └── doc2: "Hello World"
// └── bob
//     └── balance: 200
```

---

## node-grove - Node.js Bindings

### Overview
Provides complete GroveDB functionality to JavaScript/Node.js applications through native bindings using NAPI-RS.

### Architecture
- **Thread Safety**: Dedicated thread for GroveDB operations
- **Message Passing**: Commands sent via channels
- **Async API**: Non-blocking JavaScript interface

### JavaScript API
```javascript
const GroveDB = require('node-grove');

// Open database
const db = new GroveDB('./db');

// Insert data
await db.insert(
    ['users', 'alice'],
    'balance',
    { type: 'item', value: Buffer.from('100') }
);

// Query data
const result = await db.get(['users', 'alice'], 'balance');

// Start transaction
const tx = await db.startTransaction();
await db.insert(['users', 'bob'], 'balance', { type: 'item', value: Buffer.from('200') }, tx);
await db.commitTransaction(tx);

// Execute path query
const query = {
    path: ['users'],
    query: {
        items: [{ type: 'range', from: 'a', to: 'z' }],
        limit: 10
    }
};
const results = await db.query(query);
```

### Type Conversions
- Rust `Vec<u8>` ↔ JavaScript `Buffer`
- Rust `Element` ↔ JavaScript object with type field
- Rust errors ↔ JavaScript exceptions

### Features
- Full transaction support
- Batch operations
- Path queries
- Proof generation and verification
- Cost tracking
- Async/await interface

---

## Integration and Design Patterns

### Common Patterns Across Crates

1. **Zero-Copy Design**: Path crate avoids allocations
2. **Version Safety**: Explicit version checking throughout
3. **Cost Awareness**: All operations track resource usage
4. **Type Safety**: Strong typing with Rust's type system
5. **Error Propagation**: Consistent error handling

### Crate Interactions

```
GroveDB Core
    ├── Uses grovedb-path for navigation
    ├── Checks grovedb-version for compatibility
    ├── Tracks costs with grovedb-costs
    ├── Manages storage with epoch flags
    ├── Debugs with grovedb-visualize
    └── Exposes to JS via node-grove
```

### Design Philosophy

The auxiliary crates follow consistent principles:
- **Modularity**: Each crate has a single, well-defined purpose
- **Performance**: Minimize allocations and overhead
- **Safety**: Use Rust's type system for correctness
- **Usability**: Provide ergonomic APIs
- **Extensibility**: Easy to add new features

These auxiliary crates provide essential functionality that makes GroveDB a complete, production-ready database system suitable for blockchain and other applications requiring authenticated data structures.