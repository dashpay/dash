# GroveDB - Hierarchical Authenticated Data Structure

## Overview

GroveDB is a hierarchical authenticated data structure that implements a "grove" - a tree of trees. It combines multiple Merkle AVL trees (Merk) into a unified database with cryptographic proofs, efficient secondary indexing, and support for complex data types and aggregations.

## Core Architecture

### The Grove Concept

GroveDB organizes data as a hierarchy where each node can be either:
- A data element (Item, Reference, SumItem)
- A container for more data (Tree, SumTree, CountTree)

This creates a natural hierarchical structure:
```
root
├── users (Tree)
│   ├── alice (Tree)
│   │   ├── balance (SumItem: 100)
│   │   └── documents (Tree)
│   │       ├── doc1 (Item: {...})
│   │       └── doc2 (Item: {...})
│   └── bob (Tree)
│       └── balance (SumItem: 200)
└── totals (SumTree)
    └── user_balances (Reference: /users)
```

### Element Types

GroveDB supports 8 element types, each serving specific purposes:

#### Basic Elements
```rust
pub enum Element {
    Item(Vec<u8>, Option<ElementFlags>),
    Reference(ReferencePathType, MaxReferenceHop, Option<ElementFlags>),
    Tree(Option<Vec<u8>>, Option<ElementFlags>),
    // ... aggregate types
}
```

1. **Item**: Basic key-value storage
   - Stores arbitrary byte data
   - Most common element type
   - Supports optional flags

2. **Reference**: Links between elements
   - 7 reference types for different relationships
   - Configurable hop limits (default: 10)
   - Automatic cycle detection

3. **Tree**: Container for subtrees
   - Creates new hierarchical level
   - Can store root hash value
   - Enables natural data organization

#### Aggregate Elements

4. **SumItem**: Summable value
   - Stores i64 value
   - Contributes to parent SumTree totals
   - Used for balances, counts, etc.

5. **SumTree**: Tree with sum tracking
   - Automatically sums all SumItem descendants
   - Maintains sum during updates
   - Supports negative values

6. **BigSumTree**: Large sum support
   - Uses 256-bit integers
   - For values exceeding i64 range

7. **CountTree**: Element counting
   - Tracks number of elements
   - Useful for pagination, statistics

8. **CountSumTree**: Combined counting and summing
   - Maintains both count and sum
   - Efficient for aggregated statistics

### Reference System

GroveDB's sophisticated reference system enables complex data relationships:

```rust
pub enum ReferencePathType {
    AbsolutePathReference(Vec<Vec<u8>>),
    UpstreamRootHeightReference(u8, Vec<Vec<u8>>),
    UpstreamRootHeightWithParentPathAdditionReference(u8, Vec<Vec<u8>>),
    UpstreamFromElementHeightReference(u8, Vec<Vec<u8>>),
    CousinReference(Vec<Vec<u8>>),
    SiblingReference(Vec<u8>),
    UtilityReference(UtilityReferenceType),
}
```

**Reference Types Explained**:
- **Absolute**: Direct path from root
- **Upstream**: Navigate up N levels then follow path
- **Cousin**: Reference elements at same tree level
- **Sibling**: Reference within same parent
- **Utility**: Special-purpose references

### Operations

#### Insert Operations
```rust
pub fn insert<B: AsRef<[u8]>>(
    &self,
    path: SubtreePath<B>,
    key: &[u8],
    element: Element,
    options: InsertOptions,
    transaction: TransactionArg,
    grove_version: &GroveVersion,
) -> CostResult<(), Error>
```

**Features**:
- Validates element types and paths
- Updates aggregate values automatically
- Propagates root hashes upward
- Supports conditional insertion

#### Delete Operations
- **delete**: Standard deletion
- **delete_up_tree**: Remove empty parents recursively
- **clear_subtree**: Efficient bulk deletion

#### Query System

The `PathQuery` system enables sophisticated data retrieval:

```rust
pub struct PathQuery {
    pub path: Vec<Vec<u8>>,
    pub query: SizedQuery,
}

pub struct SizedQuery {
    pub query: Query,
    pub limit: Option<u16>,
    pub offset: Option<u16>,
}

pub struct Query {
    pub items: Vec<QueryItem>,
    pub default_subquery_branch: SubqueryBranch,
    pub conditional_subquery_branches: Option<IndexMap<QueryItem, SubqueryBranch>>,
    pub left_to_right: bool,
    pub add_parent_tree_on_subquery: bool,  // New in v2
}
```

**Query Features**:
- Range queries with various operators
- Subqueries based on element types
- Left-to-right or right-to-left traversal
- Pagination with limit/offset
- **Parent tree inclusion**: When `add_parent_tree_on_subquery` is true, the parent tree element itself is included in query results when performing subqueries

**Query Items**:
- `Key`: Exact key match
- `Range`: Key range queries
- `RangeInclusive`: Inclusive ranges
- `RangeFull`: All keys
- Complex range variants for advanced queries

**Parent Tree Inclusion (v2+)**:
The `add_parent_tree_on_subquery` flag allows including parent tree elements in query results:
```rust
let mut query = Query::new();
query.insert_key(b"users".to_vec());
query.set_subquery(Query::new_range_full());
query.add_parent_tree_on_subquery = true;  // Include "users" tree in results

let path_query = PathQuery::new_unsized(vec![], query);
// Results will include both the "users" tree element AND its contents
```

### Batch Operations

GroveDB provides atomic batch operations:

```rust
pub enum GroveDbOp {
    InsertTreeWithRootHash { hash: [u8; 32], .. },
    InsertOnly { element: Element },
    InsertOrReplace { element: Element },
    Replace { element: Element },
    Delete,
    DeleteTree,
    DeleteUpTree { stop_path_height: Option<u16> },
    TransientInsertTreeWithRootHash { hash: [u8; 32], .. },
}
```

**Batch Processing**:
1. Validation phase checks all operations
2. Application phase executes atomically
3. Root hash propagation through TreeCache
4. Transaction rollback on any failure

### Proof System

#### Proof Generation
```rust
pub fn prove_query(
    &self,
    path_query: &PathQuery,
    prove_options: Option<ProveOptions>,
    transaction: TransactionArg,
    grove_version: &GroveVersion,
) -> CostResult<GroveDBProof, Error>
```

**Proof Structure**:
- Layer-by-layer Merkle proofs
- Handles references by including values
- Supports absence proofs
- Configurable proof options

#### Proof Verification
```rust
pub fn verify_query(
    proof: &[u8],
    path_query: &PathQuery,
    grove_version: &GroveVersion,
) -> Result<(RootHash, Vec<Element>), Error>
```

**Verification Process**:
1. Deserialize proof data
2. Verify each layer's Merkle proof
3. Follow references as needed
4. Return root hash and matching elements

### Transaction Support

GroveDB uses RocksDB's transaction system:

```rust
// Start transaction
let tx = db.start_transaction();

// Perform operations
db.insert(path1, key1, element1, None, Some(&tx), grove_version)?;
db.insert(path2, key2, element2, None, Some(&tx), grove_version)?;

// Commit atomically
db.commit_transaction(tx)?;
```

**Transaction Features**:
- ACID properties
- Isolation between concurrent operations
- Automatic rollback on errors
- Cost tracking across transaction

### Version Management

GroveDB uses versioning for compatibility:

```rust
check_grovedb_v0_with_cost!(
    "insert",
    grove_version.grovedb_versions.operations.insert.insert
);
```

**Version Support**:
- Protocol version checking
- Feature-specific versioning
- Smooth upgrade paths
- Backward compatibility

## Advanced Features

### MerkCache System

Improves performance by caching frequently accessed subtrees:
- Reduces tree opening overhead
- Batch root hash propagation
- Configurable cache size
- Automatic eviction

### Cost Tracking

Comprehensive resource accounting:
```rust
pub struct OperationCost {
    pub seek_count: u64,
    pub storage_cost: StorageCost,
    pub storage_loaded_bytes: u64,
    pub hash_node_calls: u64,
}
```

All operations return costs for:
- Database seeks
- Storage bytes added/removed
- Computational work
- Memory usage

### Element Flags

Optional metadata for elements:
```rust
pub struct ElementFlags {
    pub epoch: Option<u64>,
    pub owner_id: Option<[u8; 32]>,
    pub storage_flags: Option<StorageFlags>,
}
```

Enables:
- Epoch-based storage management
- Multi-tenancy with owner tracking
- Custom storage policies

## Usage Examples

### Basic Usage
```rust
// Create database
let db = GroveDb::open("./db")?;

// Insert data
db.insert(
    &["users", "alice"],
    b"balance",
    Element::new_item(b"100"),
    None,
    None,
    grove_version,
)?;

// Query data
let result = db.get(&["users", "alice"], b"balance", None, grove_version)?;

// Create reference
db.insert(
    &["indexes", "by_balance"],
    b"alice",
    Element::new_reference(ReferencePathType::absolute_path(vec![
        b"users".to_vec(),
        b"alice".to_vec(),
    ])),
    None,
    None,
    grove_version,
)?;
```

### Complex Queries
```rust
// Create path query
let mut query = Query::new();
query.insert_range(b"a".to_vec()..b"z".to_vec());

let path_query = PathQuery::new(
    vec![b"users".to_vec()],
    SizedQuery::new(query, Some(10), None),
);

// Execute query
let (elements, _) = db.query(&path_query, false, false, None, grove_version)?;

// Generate proof
let proof = db.prove_query(&path_query, None, None, grove_version)?;
```

### Batch Operations
```rust
// Create batch
let ops = vec![
    GroveDbOp::insert_op(
        vec![b"users".to_vec()],
        b"charlie",
        Element::new_tree(None),
    ),
    GroveDbOp::insert_op(
        vec![b"users".to_vec(), b"charlie".to_vec()],
        b"balance",
        Element::new_sum_item(50),
    ),
];

// Apply batch
db.apply_batch(ops, None, None, grove_version)?;
```

## Design Philosophy

GroveDB is designed with several core principles:

1. **Hierarchical by Nature**: Natural tree-of-trees structure
2. **Cryptographically Secure**: Every operation maintains proof integrity
3. **Performance Optimized**: Batch operations, caching, lazy loading
4. **Feature Rich**: Multiple element types, references, aggregations
5. **Cost Aware**: Detailed resource tracking for blockchain use

## Integration Points

### With Merk
- Each Tree element contains a Merk instance
- GroveDB coordinates multiple Merk trees
- Root hashes propagate through hierarchy

### With Storage
- Prefixed storage isolates subtrees
- Transactions span multiple trees
- Cost tracking flows through layers

### With Applications
- Natural API for hierarchical data
- Efficient secondary indexing
- Cryptographic proofs for verification
- Cost estimation for fee calculation

## Future Directions

- Concurrent operation support
- Additional aggregate types
- Query language enhancements
- Improved proof compression
- Performance optimizations