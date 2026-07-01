# Merk - High-Performance Merkle AVL Tree

## Overview

Merk is a high-performance Merkle AVL tree implementation that combines the self-balancing properties of AVL trees with cryptographic hashing for verifiable data structures. It serves as the fundamental building block for GroveDB's authenticated data storage.

## Architecture

### Core Components

#### TreeNode Structure
```rust
pub struct TreeNode {
    pub inner: Box<TreeNodeInner>,
    pub old_value: Option<Vec<u8>>,
    pub known_storage_cost: Option<u32>,
}

pub struct TreeNodeInner {
    pub left: Option<Link>,
    pub right: Option<Link>,
    pub kv: KV,
}
```

- **TreeNode**: The primary node structure containing data and child links
- **KV**: Stores key, value, hashes, and feature type (Sum, Count, etc.)
- **Link**: Smart pointer system for memory-efficient tree management

#### Link System

The `Link` enum provides four states for node connections:
1. **Reference**: Pruned node with only key and hash (memory-efficient)
2. **Modified**: Changed node awaiting hash computation
3. **Uncommitted**: Modified with up-to-date hash
4. **Loaded**: Unmodified node in memory

This design enables:
- Lazy loading of nodes from storage
- Efficient memory usage through pruning
- Tracking of modifications for batch updates

### Tree Operations

#### Insert/Update Operations
- **Put**: Standard key-value insertion with AVL rebalancing
- **PutWithSpecializedCost**: Fixed-size values with predefined costs
- **PutCombinedReference**: Insert references to other elements
- **PutLayeredReference**: Insert references to subtrees

#### Delete Operations
- Removes nodes while maintaining AVL balance
- Promotes edge nodes when deleting nodes with two children
- Handles specialized deletions for sum/count trees

#### Balancing Algorithm
- Maintains AVL invariant: balance factor ∈ {-1, 0, 1}
- Balance factor = right_height - left_height
- Single and double rotations for rebalancing
- O(log n) guaranteed operation complexity

### Proof System

#### Proof Generation
Merk generates cryptographic proofs using a stack-based virtual machine:

**Operations**:
- `Push`/`PushInverted`: Add nodes to stack
- `Parent`/`Child`: Build tree relationships
- `ParentInverted`/`ChildInverted`: Handle reversed traversals

**Node Types in Proofs**:
- `Hash`: Just the node hash (32 bytes)
- `KVHash`: Hash of key-value pair
- `KV`: Full key and value
- `KVValueHash`: Key, value, and value hash
- `KVDigest`: Key and value hash (no value)

#### Proof Verification
- Stack-based execution of proof operations
- Reconstructs tree structure and validates hashes
- Supports absence proofs for non-existing keys

### Aggregate Features

Merk supports aggregated data across subtrees:

#### Sum Trees
- **SumTree**: Tracks sum of i64 values
- **BigSumTree**: Supports i128 for larger sums
- Sums propagate automatically during updates

#### Count Trees
- **CountTree**: Maintains element count
- **CountSumTree**: Combines counting and summing

These features enable efficient computation of aggregates without traversing entire subtrees.

### Storage Abstraction

The `Fetch` trait abstracts storage access:
```rust
pub trait Fetch {
    fn fetch(&self, link: &Link, ...) -> CostResult<TreeNode, Error>;
}
```

Benefits:
- Support for different storage backends
- Enables chunk-based restoration
- Allows in-memory and persistent implementations

### Walker Pattern

The `Walker` provides lazy traversal with automatic node fetching:
```rust
pub struct Walker<T> {
    tree: TreeNode,
    source: T,
}
```

Features:
- Wraps tree with data source
- Fetches pruned nodes on-demand
- Enables operations on partially-loaded trees

### Chunk System

For large tree restoration:
- Trees broken into chunks at configurable depths
- Each chunk independently verifiable
- Parallel restoration and verification
- Uses left/right traversal instructions

### Cost Tracking

Comprehensive resource accounting:
- **Storage costs**: Added/replaced/removed bytes
- **Operation costs**: Computational work
- **Value-defined costs**: Predefined costs for special nodes
- Cost accumulation through all operations

### Encoding

Binary encoding using the `ed` crate:
- Compact representation for storage
- Variable-length encoding for efficiency
- Key size limit: 255 bytes (u8 length prefix)
- Separate encoding for tree structure vs links

## Performance Characteristics

### Time Complexity
- Insert/Delete/Search: O(log n)
- Proof generation: O(log n)
- Proof verification: O(log n)

### Space Complexity
- Tree storage: O(n)
- Proof size: O(log n)
- Memory usage: Configurable through pruning

### Optimizations
1. **Lazy Loading**: Nodes fetched only when needed
2. **Batch Operations**: Multiple updates in single traversal
3. **Memory Pruning**: Configurable retention of loaded nodes
4. **Cost Caching**: Avoid recalculating known costs
5. **Reference Counting**: Efficient memory management

## Usage Examples

### Basic Operations
```rust
// Create a new Merk tree
let mut merk = Merk::new();

// Insert key-value pair
merk.put(b"key", b"value", None)?;

// Get value
let value = merk.get(b"key")?;

// Delete key
merk.delete(b"key")?;

// Generate proof
let proof = merk.prove_query(query)?;
```

### Batch Operations
```rust
// Create batch
let mut batch = merk.batch();

// Add operations
batch.put(b"key1", b"value1")?;
batch.put(b"key2", b"value2")?;
batch.delete(b"key3")?;

// Apply batch
merk.apply_batch(batch)?;
```

## Design Philosophy

Merk is designed for:
1. **Cryptographic Security**: Every operation maintains Merkle tree integrity
2. **Performance**: Optimized for blockchain workloads
3. **Memory Efficiency**: Pruning and lazy loading for large datasets
4. **Flexibility**: Supports various node types and aggregations
5. **Verifiability**: Efficient proof generation and verification

## Integration with GroveDB

Merk serves as the storage engine for each subtree in GroveDB:
- Each GroveDB tree element contains a Merk instance
- Root hashes propagate through the hierarchy
- Proofs combine across multiple Merk trees
- Storage contexts isolate different subtrees

## Future Considerations

- Support for concurrent operations
- Additional aggregate types
- Optimized proof compression
- Enhanced chunk restoration strategies