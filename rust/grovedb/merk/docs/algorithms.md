# Merk - A High-Performance Merkle AVL Tree

**Matt Bell ([@mappum](https://twitter.com/mappum))** • [Nomic Hodlings, Inc.](https://nomic.io)

v0.0.4 - _August 5, 2020_

## Introduction

Merk is a Merkle AVL tree designed for performance, running on top of a backing key/value store such as RocksDB. Notable features include concurrent operations for higher throughput, an optimized key/value layout for performant usage of the backing store, and efficient proof generation to enable bulk tree replication.

_Note that this document is meant to be a way to grok how Merk works, rather than an authoritative specification._

## Algorithm Overview

The Merk tree was inspired by [`tendermint/iavl`](https://github.com/tendermint/iavl) from the [Tendermint](https://tendermint.com) team but makes various fundamental design changes in the name of performance.

### Tree Structure

#### Nodes and Hashing

In many Merkle tree designs, only leaf nodes contain key/value pairs (inner nodes only contain child hashes). To contrast, every node in a Merk tree contains a key and a value, including inner nodes.

Each node contains a "kv hash", which is the hash of its key/value pair, in addition to its child hashes. The hash of the node is just the hash of the concatenation of these three hashes:

```
kv_hash = H(varint(key.len()) || key || H(value))
node_hash = H(kv_hash || left_child_hash || right_child_hash)
```

Note that the `left_child_hash` and/or `right_child_hash` values may be null since it is possible for the node to have no children or only one child.

In our implementation, the hash function used is Blake2b (chosen for its performance characteristics) but this choice is trivially swappable.

#### Database Representation

In the backing key/value store, nodes are stored using their key/value pair key as the database key, and a binary encoding that contains the fields in the above `Node` structure - minus the `key` field since that is already implied by the database entry.

Storing nodes by key rather than by hash is an important optimization, and is the reason why inner nodes each have a key/value pair. The implication is that reading a key does not require traversing through the tree structure but only requires a single read in the backing key/value store, meaning there is practically no overhead versus using the backing store without a tree structure. Additionally, we can efficiently iterate through nodes in the tree in their in-order traversal just by iterating by key in the backing store (which RocksDB and LevelDB are optimized for).

This means we lose the "I" compared to the IAVL library - immutability. Since now we operate on the tree nodes in-place in the backing store, we don't by default have views of past states of the tree. However, **in** our implementation we replicate this functionality with RocksDB's snapshot and checkpoint features which provide a consistent view of the store at a certain point in history - either ephemerally in memory or persistently on disk.

### Operations

Operating on a Merk tree is optimized for batches - in the real world we will only be updating the tree once per block, applying a batch of many changes from many transactions at the same time.

#### Concurrent Batch Operator

To mutate the tree, we apply batches of operations, each of which can either be `Put(key, value)` or `Delete(key)`.

Batches of operations are expected to be sorted by key, with every key appearing only once. Our implementation provides an `apply` method which sorts the batch and checks for duplicate keys, and an `apply_unchecked` method which skips the sorting/checking step for performance reasons when the caller has already ensured the batch is sorted.

The algorithm to apply these operations to the tree is called recursively on each relevant node.

_Simplified pseudocode for the operation algorithm:_

- Given a node and a batch of operations:
  - Binary search for the current node's key in the batch:
    - If this node's key is found in the batch at index `i`:
      - Apply the operation to this node:
        - If operation is `Put`, update its `value` and `kv_hash`
        - If the operation is `Delete`, perform a traditional BST node removal
      - Split the batch into left and right sub-batches (excluding the operation we just applied):
        - Left batch from batch start to index `i`
        - Right batch from index `i + 1` to the end of the batch
    - If this node's key is not found in the batch, but could be inserted at index `i` maintaining sorted order:
      - Split the batch into left and right sub-batches:
        - Left batch from batch start to index `i`
        - Right batch from index `i` to the end of the batch
  - Recurse:
    - Apply the left sub-batch to this node's left child
    - Apply the right sub-batch to this node's right child
  - Balance:
    - If after recursing the left and right subtrees are unbalanced (their heights differ by more than 1), perform an AVL tree rotation (possibly more than one)
  - Recompute node's hash based on hash of its updated children and `kv_hash`, then return

This batch application of operations can happen concurrently - recursing into the left and right subtrees of a node are two fully independent operations (operations on one subtree will never involve reading or writing to/from any of the nodes on the other subtree). This means we have an _implicit lock_ - we don't need to coordinate with mutexes but only need to wait for both the left side and right side to finish their operations.

### Proofs

Merk was designed with efficient proofs in mind, both for application queries (e.g. a user checking their account balance) and bulk tree replication (a.k.a. "state syncing") between untrusted nodes.

#### Structure

Merk proofs are a list of stack-based operators and node data, with 3 possible operators: `Push(node)`, `Parent`, and `Child`. A stream of these operators can be processed by a verifier in order to reconstruct a sparse representation of part of the tree, in a way where the data can be verified against a known root hash.

The value of `node` in a `Push` operation can be one of three types:

- `Hash(hash)` - The hash of a node
- `KVHash(hash)` - The key/value hash of a node
- `KV(key, value)` - The key and value of a node

This proof format can be encoded in a binary format and has negligible space overhead for efficient transport over the network.

#### Verification

A verifier can process a proof by maintaining a stack of connected tree nodes, and executing the operators in order:

- `Push(node)` - Push some node data onto the stack.
- `Child` - Pop a value from the stack, `child`. Pop another value from the stack, `parent`. Set `child` as the right child of `parent`, and push the combined result back on the stack.
- `Parent` - Pop a value from the stack, `parent`. Pop another value from the stack, `child`. Set `child` as the left child of `parent`, and push the combined result back on the stack.

Proof verification will fail if e.g. `Child` or `Parent` try to pop a value from the stack but the stack is empty, `Child` or `Parent` try to overwrite an existing child, or the proof does not result in exactly one stack item.

This proof language can be used to specify any possible set or subset of the tree's data in a way that can be reconstructed efficiently by the verifier. Proofs can contain either an arbitrary set of selected key/value pairs (e.g. in an application query), or contiguous tree chunks (when replicating the tree). After processing an entire proof, the verifier should have derived a root hash which can be compared to the root hash they expect (e.g. the one validators committed to in consensus), and have a set of proven key/value pairs.

Note that this can be computed in a streaming fashion, e.g. while downloading the proof, which makes the required memory for verification very low even for large proofs. However, the verifier cannot tell if the proof is valid until finishing the entire proof, so very large proofs should be broken up into multiple proofs of smaller size.

#### Generation

Efficient proof generation is important since nodes will likely receive a high volume of queries and constantly be serving proofs, essentially providing an API service to end-user application clients, as well as servicing demand for replication when new nodes come onto the network.

Nodes can generate proofs for a set of keys by traversing through the tree from the root and building up the required proof branches. Much like the batch operator aglorithm, this algorithm takes a batch of sorted, unique keys as input.

_Simplified pseudocode for proof generation (based on an in-order traversal):_

- Given a node and a batch of keys to include in the proof:
  - If the batch is empty, append `Push(Hash(node_hash))` to the proof and return
  - Binary search the for the current node's key in the batch:
    - If this node's key is found in the batch at index `i`:
      - Partition the batch into left and right sub-batches at index `i` (excluding index `i`)
    - If this node's key is not found in the batch, but could be inserted at index `i` maintaining sorted order:
      - Partition the batch into left and right sub-batches at index `i`
  - **Recurse left:** If there is a left child:
    - If the left sub-batch is not empty, query the left child (appending operators to the proof)
    - If the left sub-batch is empty, append `Push(Hash(left_child_hash))` to the proof
  - Append proof operator:
    - If this node's key is in the batch, or if the left sub-batch was not empty and no left child exists, or if the right sub-batch is not empty and no right child exists,or if the left child's right edge queried a non-existent key, or if the right child's left edge queried a non-existent key, append `Push(KV(key, value))` to the proof
    - Otherwise, append `Push(KVHash(kv_hash))` to the proof
  - If the left child exists, append `Parent` to the proof
  - **Recurse right:** If there is a right child:
    - If the right sub-batch is not empty, query the right child (appending operators to the proof)
    - If the right sub-batch is empty, append `Push(Hash(left_child_hash))` to the proof
    - Append `Child` to the proof

Since RocksDB allows concurrent reading from a consistent snapshot/checkpoint, nodes can concurrently generate proofs on all cores to service a higher volume of queries, even if our algorithm isn't designed for concurrency.

#### Binary Format

We can efficiently encode these proofs by encoding each operator as follows:

```
Push(Hash(hash)) => 0x01 <20-byte hash>
Push(KVHash(hash)) => 0x02 <20-byte hash>
Push(KV(key, value)) => 0x03 <1-byte key length> <n-byte key> <2-byte value length> <n-byte value>
Parent => 0x10
Child => 0x11
```

This results in a compact binary representation, with a very small space overhead (roughly 2 bytes per node in the proof (1 byte for the `Push` operator type flag, and 1 byte for a `Parent` or `Child` operator), plus 3 bytes per key/value pair (1 byte for the key length, and 2 bytes for the value length)).

#### Efficient Chunk Proofs for Replication

An alternate, optimized proof generation can be used when generating proofs for large contiguous subtrees, e.g. chunks for tree replication. This works by iterating sequentially through keys in the backing store (which is much faster than random lookups).

Based on some early benchmarks, I estimate that typical server hardware should be able to generate this kind of range proof at a rate of hundreds of MB/s, which means the bottleneck for bulk replication will likely be bandwidth rather than CPU. To improve performance further, these proofs can be cached and trivially served by a CDN or a P2P swarm (each node of which can easily verify the chunks they pass around).

Due to the tree structure we already use, streaming the entries in key-order gives us all the nodes to construct complete contiguous subtrees. For instance, in the diagram below, streaming from keys `1` to `7` will give us a complete subtree. This subtree can be verified to be a part of the full tree as long as we know the hash of `4`.

```
             8
           /   \
        /      ...
      4
    /   \
  2       6
 / \     / \
1   3   5   7
```

Our algorithm builds verifiable chunks by first constructing a chunk of the upper levels of the tree, called the _trunk chunk_, plus each subtree below that (each of which is called a _leaf chunk_).

The number of levels to include in the trunk can be chosen to control the size of the leaf nodes. For example, a tree of height 10 should have approximately 1,023 nodes. If the trunk contains the top 5 levels, the trunk and the 32 resulting leaf nodes will each contain ~31 nodes. We can even prove to the verifier the trunk size was chosen correctly by also including an approximate tree height proof, by including the branch all the way to the leftmost node of the tree (node `1` in the figure) and using this height as our basis to select the number of trunk levels.

After the prover builds the trunk by traversing from the root node and making random lookups down to the chosen level, it can generate the leaf nodes extremely efficiently by reading the database keys sequentially as described a few paragraphs above. We can trivially detect when a chunk should end whenever a node at or above the trunk level is encountered (e.g. encountering node `8` signals we have read a complete subtree).

The generated proofs can be efficiently encoded into the same proof format described above. Verifiers only have the added constraint that none of the data should be abbridged (all nodes contain a key and value, rather than just a hash or kvhash). After first downloading and verifying the trunk, verifiers can also download leaf chunks in parallel and verify that each connects to the trunk by comparing each subtree's root hash.

Note that this algorithm produces proofs with very little memory requirements, plus little overhead added to the sequential read from disk. In a proof-of-concept benchmark, proof generation was measured to be ~750 MB/s on a modern solid-state drive and processor, meaning a 4GB state tree (the size of the Cosmos Hub state at the time of writing) could be fully proven in ~5 seconds (without considering parallelization). In conjunction with the RocksDB checkpoint feature, this process can happen in the background without blocking the node from executing later blocks.

_Pseudocode for the range proof generation algorithm:_

- Given a tree and a range of keys to prove:
  - Create a stack of keys (initially empty)
  - **Range iteration:** for every key/value entry within the query range in the backing store:
    - Append `Push(KV(key, value))` to the proof
    - If the current node has a left child, append `Parent` to the proof
    - If the current node has a right child, push the right child's key onto the key stack
    - If the current node does not have a right child:
      - While the current node's key is greater than or equal to the key at the top of the key stack, append `Child` to the proof and pop from the key stack

Note that this algorithm produces the proof in a streaming fashion and has very little memory requirements (the only overhead is the key stack, which will be small even for extremely large trees since its length is a maximum of `log N`).

#### Example Proofs

Let's walk through a concrete proof example. Consider the following tree:

```
       5
      / \
    /     \
  2        9
 / \      /  \
1   4    7    11
   /    / \   /
  3    6   8 10
```

_Small proof:_

First, let's create a proof for a small part of the tree. Let's say the user makes a query for keys `1, 2, 3, 4`.

If we follow our proof generation algorithm, we should get a proof that looks like this:

```
Push(KV(1, <value of 1>)),
Push(KV(2, <value of 2>)),
Parent,
Push(KV(3, <value of 3>)),
Push(KV(4, <value of 4>)),
Parent,
Child,
Push(KVHash(<kv_hash of 5>)),
Parent,
Push(Hash(<hash of 9>)),
Child
```

Let's step through verification to show that this proof works. We'll create a verification stack, which starts out empty, and walk through each operator in the proof, in order:

```
Stack: (empty)
```

We will push a key/value pair on the stack, creating a node. However, note that for verification purposes this node will only need to contain the kv_hash which we will compute at this step.

```
Operator: Push(KV(1, <value of 1>))

Stack:
1
```

```
Operator: Push(KV(2, <value of 2>))

Stack:
1
2
```

Now we connect nodes 1 and 2, with 2 as the parent.

```
Operator: Parent

Stack:
  2
 /
1
```

```
Operator: Push(KV(3, <value of 3>))

Stack:
  2
 /
1
3
```

```
Operator: Push(KV(4, <value of 4>))

Stack:
  2
 /
1
3
4
```

```
Operator: Parent

Stack:
  2
 /
1
  4
 /
3
```

Now connect these two graphs with 4 as the child of 2.

```
Operator: Child

Stack:
  2
 / \
1   4
   /
  3
```

Since the user isn't querying the data from node 5, we only need its kv_hash.

```
Operator: Push(KVHash(<kv_hash of 5>))

Stack:
  2
 / \
1   4
   /
  3
5
```

```
Operator: Parent

Stack:
    5
   /
  2
 / \
1   4
   /
  3
```

We only need the hash of node 9.

```
Operator: Push(Hash(<hash of 9>))

Stack:
    5
   /
  2
 / \
1   4
   /
  3
9
```

```
Operator: Child

Stack:
    5
   / \
  2   9
 / \
1   4
   /
  3
```

Now after going through all these steps, we have sufficient knowlege of the tree's structure and data to compute node hashes in order to verify. At the end, we will have computed a hash for node 5 (the root), and we verify by comparing this hash to the one we expected.
