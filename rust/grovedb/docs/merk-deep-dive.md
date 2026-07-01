# Merk Deep Dive: Nodes, Proofs, and State

## Table of Contents
- [What is a Node in Merk?](#what-is-a-node-in-merk)
- [Node Structure and Components](#node-structure-and-components)
- [What Can Be Proved?](#what-can-be-proved)
- [What Cannot Be Proved?](#what-cannot-be-proved)
- [Proof Node Types](#proof-node-types)
- [How Proofs Work](#how-proofs-work)
- [Examples](#examples)

## What is a Node in Merk?

In Merk (Merkle AVL tree), every node is both:
1. **A data container**: Stores a key-value pair
2. **A tree structure element**: Has left and right children (or none)
3. **An authenticated element**: Contains cryptographic hashes

Unlike traditional Merkle trees where only leaves store data, in Merk **every node stores data**, making it more space-efficient.

## Node Structure and Components

### Core Node Structure

```rust
pub struct TreeNode {
    pub inner: Box<TreeNodeInner>,
    pub old_value: Option<Vec<u8>>,      // Previous value for cost tracking
    pub known_storage_cost: Option<u32>,  // Cached storage cost
}

pub struct TreeNodeInner {
    pub left: Option<Link>,   // Left child
    pub right: Option<Link>,  // Right child
    pub kv: KV,              // Key-value data
}

pub struct KV {
    pub key: Vec<u8>,                    // The node's key
    pub value: Vec<u8>,                  // The node's value
    pub hash: Hash,                      // Hash of entire node
    pub value_hash: ValueHash,           // Hash of just the value
    pub feature_type: TreeFeatureType,   // Node type (basic, sum, etc.)
}
```

### Hash Computation

Each node computes its hash as:
```
value_hash = Hash(value)
kv_hash = Hash(varint(key.len()) || key || value_hash)
node_hash = Hash(kv_hash || left_child_hash || right_child_hash)
```

This creates a chain of authentication from leaves to root.

## What Can Be Proved?

Merk can generate cryptographic proofs for:

### 1. **Membership Proofs**
Prove that a key-value pair exists in the tree:
- "Key X has value Y"
- "Key X exists" (without revealing the value)
- "Keys in range [A, B] exist with these values"

### 2. **Non-Membership Proofs**
Prove that a key does NOT exist in the tree:
- "Key X is not in the tree"
- "No keys exist in range [A, B]"

### 3. **Range Proofs**
Prove all keys within a range:
- "All keys between 'alice' and 'bob' are: [alice, amanda, bob]"
- "The first 10 keys starting from 'X' are: [...]"

### 4. **Aggregate Proofs** (for special tree types)
- **Sum Trees**: "The sum of all values is 1000"
- **Count Trees**: "There are exactly 50 elements"
- **Combined**: "50 elements with total sum 1000"

### 5. **Absence Range Proofs**
Prove no keys exist in a range by showing the neighboring keys:
- "No keys exist between 'cat' and 'dog' (here are the adjacent keys)"

### 6. **Hash Proofs**
Prove the tree has a specific root hash:
- "The tree with root hash H contains key K with value V"

## What Cannot Be Proved?

Merk has limitations on what can be efficiently proved:

### 1. **Historical State**
- Cannot prove what a value WAS (only current state)
- Cannot prove when a value changed
- Cannot prove the history of modifications

### 2. **Complex Queries Without Indices**
- Cannot efficiently prove "all keys with value > 100" unless indexed
- Cannot prove "all keys matching pattern X" without traversing
- Cannot prove aggregations on non-indexed attributes

### 3. **Negative Queries on Values**
- Cannot prove "no key has value X" without full tree traversal
- Cannot prove "all values are unique"

### 4. **Metadata**
- Cannot prove when a key was inserted
- Cannot prove who inserted a key
- Cannot prove access patterns

### 5. **Relative Proofs Without Context**
- Cannot prove "key X has the 5th largest value"
- Cannot prove "key X appears before key Y" without range context

## Proof Node Types

When generating proofs, Merk uses different node representations to optimize proof size:

### 1. **Hash**
```rust
Node::Hash(hash: [u8; 32])
```
- **Purpose**: Proves a subtree exists without revealing contents
- **Size**: 32 bytes
- **Use Case**: When you need to verify tree structure but not the data
- **Example**: Proving a path to a specific key without revealing sibling data

### 2. **KVHash**
```rust
Node::KVHash(kv_hash: [u8; 32])
```
- **Purpose**: Proves the hash of a key-value pair without revealing the actual data
- **Size**: 32 bytes
- **Use Case**: When you need to verify data exists but keep it private
- **Example**: Proving a node exists in a path without exposing its contents

### 3. **KV**
```rust
Node::KV(key: Vec<u8>, value: Vec<u8>)
```
- **Purpose**: Full disclosure of both key and value
- **Size**: Variable (key length + value length)
- **Use Case**: When the verifier needs to see the actual data
- **Example**: Proving "alice" has balance "100"

### 4. **KVValueHash**
```rust
Node::KVValueHash(key: Vec<u8>, value: Vec<u8>, value_hash: [u8; 32])
```
- **Purpose**: Reveals key and value plus a separate hash of just the value
- **Size**: Variable + 32 bytes
- **Use Case**: When you need to prove the value and enable value-specific operations
- **Example**: Proving data in a sum tree where value hash is used for aggregation

### 5. **KVDigest**
```rust
Node::KVDigest(key: Vec<u8>, value_hash: [u8; 32])
```
- **Purpose**: Reveals the key but only provides hash of the value
- **Size**: Key length + 32 bytes
- **Use Case**: Proving a key exists without revealing its value
- **Example**: Proving "alice" exists without showing her balance

### 6. **KVRefValueHash**
```rust
Node::KVRefValueHash(key: Vec<u8>, value_hash: [u8; 32], referenced_value: Vec<u8>)
```
- **Purpose**: For reference elements - shows key, value hash, and the referenced data
- **Size**: Key length + 32 bytes + referenced value length
- **Use Case**: Proving references in GroveDB where value points to other data
- **Example**: Proving an index entry that references actual data elsewhere

## Special Tree Types and Aggregation

### Count Trees (CountedMerkNode)

In a count tree, it's important to understand the difference between what's stored in the `TreeFeatureType` and what's computed:

**TreeFeatureType Storage**:
- `CountedMerkNode(1)` - Regular items store just their own contribution of 1
- `CountedMerkNode(n)` - CountTree elements store their specific count value

**Aggregate Computation**:
The total count is computed dynamically and stored in the `Link`'s `aggregate_data` field:

```rust
// In TreeFeatureType - stores only own contribution
CountedMerkNode(1)  // Just this node's count

// In Link - stores computed aggregate
aggregate_data: AggregateData::Count(3)  // This node + all descendants
```

Example count tree structure:
```
        root 
        TreeFeatureType: CountedMerkNode(1)
        Link.aggregate_data: Count(7)
       /                            \
   alice                          charlie
   CountedMerkNode(1)             CountedMerkNode(1)  
   aggregate_data: Count(3)       aggregate_data: Count(3)
   /            \                          \
bob           carol                       dave
CountedMerkNode(1)                     CountedMerkNode(1)
aggregate_data: Count(1)               aggregate_data: Count(1)
```

The aggregation works as follows:
- Each node's `TreeFeatureType` stores only its own count (usually 1)
- The `aggregate_data` in the Link stores: own count + left subtree aggregate + right subtree aggregate
- This aggregate data is persisted to disk but is NOT part of the authenticated state
- This allows O(1) retrieval of the total count at any node level without recomputation

**Important: Aggregate Data is NOT in the State**

While aggregate data is persisted to disk for performance, it is **NOT part of the cryptographic state**:
- The node hash is computed from: `Hash(kv_hash, left_child_hash, right_child_hash)`
- Aggregate data is NOT included in the hash computation
- Therefore, aggregate data cannot be proven with a GroveDB proof
- It's a derived value that can be recomputed from the tree structure

**Storage Layout**:
When a Link is persisted, it includes:
- Key (with length prefix)
- Hash (32 bytes) - computed WITHOUT aggregate data
- Child heights (2 bytes)
- Aggregate data type (1 byte) + value(s) - cached but not authenticated

This design separates:
- **Authenticated State**: The actual tree structure and values (provable)
- **Cached Derivatives**: Aggregate counts/sums for performance (not provable)

The precomputed storage strategy trades a small amount of extra storage space for massive query performance improvements, while keeping the authenticated state minimal.

## How Proofs Work

### Proof Generation Process

1. **Path Selection**: Identify the path from root to target key(s)
2. **Node Selection**: Choose minimal set of nodes needed for verification
3. **Node Type Selection**: Pick the most efficient node representation
4. **Encoding**: Serialize nodes with operation instructions

### Proof Verification Process

1. **Decode**: Parse the proof into nodes and operations
2. **Execute**: Run the stack-based virtual machine:
   - `Push`: Add node to stack
   - `Parent`: Combine top two nodes as parent-child
   - `Child`: Make top node a child of the next
3. **Hash Verification**: Recompute hashes and verify they match
4. **Root Validation**: Ensure final hash matches expected root

### Proof Operations

The proof uses a stack machine with operations:
- **Push/PushInverted**: Add nodes to the verification stack
- **Parent/ParentInverted**: Build parent-child relationships
- **Child/ChildInverted**: Attach children to parents

## Examples

### Example 1: Simple Membership Proof

Proving key "alice" has value "100" in this tree:
```
       root
      /    \
   alice   charlie
          /
        bob
```

Proof contains:
1. `KV("alice", "100")` - The target node
2. `Hash(charlie_subtree_hash)` - Sister subtree as hash only
3. Operations to reconstruct: `Push(alice), Push(charlie_hash), Parent`

### Example 2: Range Proof

Proving all keys from "a" to "c":
```
Proof nodes:
1. KV("alice", "100")
2. KV("bob", "200") 
3. KV("charlie", "300")
4. Hash(left_boundary)  // Proves nothing before "alice"
5. Hash(right_boundary) // Proves nothing after "charlie"
```

### Example 3: Non-Membership Proof

Proving "barbara" doesn't exist (between "alice" and "charlie"):
```
Proof contains:
1. KV("alice", "100")   // Left neighbor
2. KV("charlie", "300") // Right neighbor
3. Proof that alice's right child leads to charlie
4. No "barbara" in between
```

### Example 4: Sum Tree Proof

Proving sum of all values is 600:
```
Proof contains:
1. KVValueHash("alice", "100", hash_of_100)
2. KVValueHash("bob", "200", hash_of_200)  
3. KVValueHash("charlie", "300", hash_of_300)
4. Sum aggregation data showing 100+200+300=600
```

## Best Practices

### For Proof Generation
1. Use most compact node type that satisfies requirements
2. Batch related proofs to share common nodes
3. Consider proof size vs. verification cost trade-offs

### For Proof Verification
1. Always verify against known root hash
2. Validate all hash computations
3. Check for malformed proofs (wrong operation sequences)
4. Verify aggregate values match individual components

### For Privacy
1. Use `Hash` nodes to hide irrelevant data
2. Use `KVDigest` to prove existence without revealing values
3. Structure trees to minimize data exposure in proofs

## Conclusion

Merk's node structure and proof system provide a flexible, efficient way to prove statements about tree contents. By understanding the different node types and their purposes, developers can generate optimal proofs that balance size, privacy, and verification requirements. The inability to prove certain properties (like historical state) is a fundamental limitation of Merkle trees, but GroveDB's hierarchical structure helps overcome many limitations through careful tree organization and indexing.