//! Merk proofs

#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod branch;
#[cfg(feature = "minimal")]
pub mod chunk;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod encoding;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod query;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod tree;

#[cfg(feature = "minimal")]
pub use encoding::encode_into;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use encoding::Decoder;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use query::Query;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use tree::{execute, Tree};

#[cfg(any(feature = "minimal", feature = "verify"))]
use crate::{tree::CryptoHash, TreeFeatureType};

#[cfg(any(feature = "minimal", feature = "verify"))]
/// A proof operator, executed to verify the data in a Merkle proof.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Op {
    /// Pushes a node on the stack.
    /// Signifies ascending node keys
    Push(Node),

    /// Pushes a node on the stack
    /// Signifies descending node keys
    PushInverted(Node),

    /// Pops the top stack item as `parent`. Pops the next top stack item as
    /// `child`. Attaches `child` as the left child of `parent`. Pushes the
    /// updated `parent` back on the stack.
    Parent,

    /// Pops the top stack item as `child`. Pops the next top stack item as
    /// `parent`. Attaches `child` as the right child of `parent`. Pushes the
    /// updated `parent` back on the stack.
    Child,

    /// Pops the top stack item as `parent`. Pops the next top stack item as
    /// `child`. Attaches `child` as the right child of `parent`. Pushes the
    /// updated `parent` back on the stack.
    ParentInverted,

    /// Pops the top stack item as `child`. Pops the next top stack item as
    /// `parent`. Attaches `child` as the left child of `parent`. Pushes the
    /// updated `parent` back on the stack.
    ChildInverted,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
/// A selected piece of data about a single tree node, to be contained in a
/// `Push` operator in a proof.
///
/// Each variant carries different amounts of information, allowing proofs to
/// include only what's necessary for verification while minimizing proof size.
///
/// # Tree Structure Reference
///
/// ```text
///                    ┌───────────────────────────────────────────────────────────┐
///                    │                        Tree Node                          │
///                    │  ┌─────┐ ┌───────┐ ┌────────────────┐                     │
///                    │  │ key │ │ value │ │  feature_type  │                     │
///                    │  └──┬──┘ └───┬───┘ └───────┬────────┘                     │
///                    │     │        │             │                              │
///                    │     ▼        ▼             │                              │
///                    │  ┌──────────────┐          │                              │
///                    │  │  value_hash  │◄─────────┘                              │
///                    │  │ H(value) or  │   (combined for special trees)          │
///                    │  │ combined hash│                                         │
///                    │  └──────┬───────┘                                         │
///                    │         │                                                 │
///                    │         ▼                                                 │
///                    │  ┌─────────────────────────────────────────────────────┐  │
///                    │  │ kv_hash = H(varint(key.len()) || key || value_hash) │  │
///                    │  └────────────────────────┬────────────────────────────┘  │
///                    │                           │                               │
///                    │                           ▼                               │
///                    │  ┌─────────────────────────────────────────────────────┐  │
///                    │  │                     node_hash                       │  │
///                    │  │ H(kv_hash || left || right [|| count.to_be_bytes()])│  │
///                    │  └─────────────────────────────────────────────────────┘  │
///                    └───────────────────────────────────────────────────────────┘
/// ```
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Node {
    /// The node hash only. Used for sibling/cousin nodes not on the query path.
    ///
    /// Contains: `node_hash`
    ///
    /// ```text
    ///     Query: key "C"
    ///
    ///            [B]
    ///           /   \
    ///        [A]     [C] ◄── queried
    ///
    ///     Node [A] is included as Hash(node_hash)
    ///     - Not on query path, just need hash for parent calculation
    ///     - Reveals nothing about A's key or value
    /// ```
    ///
    /// **When used**: For nodes whose subtree is not being queried - provides
    /// the hash needed to compute the parent's node hash without revealing
    /// any key/value data.
    Hash(CryptoHash),

    /// The key-value hash only. Used for non-queried nodes on the path.
    ///
    /// Contains: `kv_hash` (hash of key concatenated with value_hash)
    ///
    /// ```text
    ///     Query: key "D"
    ///
    ///            [B] ◄── KVHash (on path, not queried)
    ///           /   \
    ///        [A]     [C] ◄── KVHash (on path, not queried)
    ///         ▲        \
    ///       Hash       [D] ◄── queried (KVValueHash)
    ///
    ///     Node [B] is included as KVHash(kv_hash)
    ///     - Ancestor of queried node, on the path to [D]
    ///     - Key/value not revealed, only their combined hash
    ///
    ///     Node [C] is also included as KVHash(kv_hash)
    ///     - Also an ancestor on the path to [D]
    ///     - Same treatment: only kv_hash revealed
    ///
    ///     Node [A] is included as Hash(node_hash)
    ///     - NOT on the path to [D], just a sibling
    ///     - Only node_hash needed for [B]'s hash calculation
    /// ```
    ///
    /// **When used**: For nodes that are ancestors of queried nodes but are not
    /// themselves being queried, in trees without provable counts. Allows
    /// computing the node hash without revealing the key or value.
    KVHash(CryptoHash),

    /// Key and value_hash. Used for proving key existence/absence at
    /// boundaries.
    ///
    /// Contains: `(key, value_hash)`
    ///
    /// ```text
    ///     Query: range ["B".."D"] (but C doesn't exist)
    ///
    ///            [B] ◄── KVDigest (left boundary)
    ///           /   \
    ///        [A]     [E] ◄── KVDigest (proves no C or D)
    ///         ▲
    ///       Hash
    ///
    ///     Nodes [B] and [E] included as KVDigest(key, value_hash)
    ///     - Key needed to prove range boundaries
    ///     - Value not returned (not requested or doesn't exist)
    /// ```
    ///
    /// **When used**: For nodes at query boundaries (proving absence of keys
    /// in a range) or proving a key exists without returning its value.
    /// The key is needed for range comparisons, but the value is not returned.
    KVDigest(Vec<u8>, CryptoHash),

    /// Key and value. Suitable for items where value_hash = H(value).
    ///
    /// Contains: `(key, value)`
    ///
    /// ```text
    ///     Query for an Item (not a subtree):
    ///
    ///            [B]
    ///           /   \
    ///        [A]     [C] ◄── KV(key, value)
    ///         ▲
    ///       Hash
    ///
    ///     Hash computation during verification:
    ///     value_hash = H(value)  ◄── computed from value
    ///     kv_hash = H(varint(key.len()) || key || value_hash)
    ///
    ///     For Items, this works correctly because the stored
    ///     value_hash in the tree is exactly H(value).
    ///
    ///     ─────────────────────────────────────────────────────
    ///
    ///     Why current proof generation uses KVValueHash instead:
    ///
    ///     ┌─────────────────────────────────────────────────┐
    ///     │  KVValueHash works for ALL element types:       │
    ///     │  - Items: value_hash = H(value) ✓               │
    ///     │  - Subtrees: value_hash = combined hash ✓       │
    ///     │                                                 │
    ///     │  KV only works for Items (not subtrees), so     │
    ///     │  KVValueHash is used uniformly for simplicity.  │
    ///     └─────────────────────────────────────────────────┘
    /// ```
    ///
    /// **When used**: Theoretically valid for queried Items where the
    /// value_hash equals `H(value)`. However, current proof generation uses
    /// `KVValueHash` uniformly for all element types. The verifier supports
    /// `KV` for compatibility, but it's not generated by GroveDB proofs.
    KV(Vec<u8>, Vec<u8>),

    /// Key, value, and value_hash. Standard node for queried items.
    ///
    /// Contains: `(key, value, value_hash)`
    ///
    /// ```text
    ///     Query: key "C" (a subtree in GroveDB)
    ///
    ///            [B]
    ///           /   \
    ///        [A]     [C] ◄── KVValueHash
    ///         ▲
    ///       Hash
    ///
    ///     Node [C] included as KVValueHash(key, value, value_hash)
    ///
    ///     For subtrees, value_hash = H(subtree_root || value_bytes)
    ///     This binding allows GroveDB to verify:
    ///     ┌──────────────────────────────────────────┐
    ///     │  Parent Tree          Child Tree        │
    ///     │  [C].value_hash  ══►  root_hash         │
    ///     │  (proves binding)     (from child proof)│
    ///     └──────────────────────────────────────────┘
    /// ```
    ///
    /// **When used**: For items that match the query in non-ProvableCountTree
    /// trees. The value_hash is included because for subtrees it may be a
    /// combined hash (e.g., subtree root hash), allowing GroveDB to verify
    /// parent-child binding across tree layers.
    KVValueHash(Vec<u8>, Vec<u8>, CryptoHash),

    /// Key, value, value_hash, and feature type. For trees with special
    /// features.
    ///
    /// Contains: `(key, value, value_hash, feature_type)`
    ///
    /// ```text
    ///     ProvableCountTree Query:
    ///
    ///            [B] count=5
    ///           /   \
    ///      [A]       [C] ◄── KVValueHashFeatureType
    ///    count=2    count=2    (key, value, value_hash,
    ///                           ProvableCountedMerkNode(2))
    ///
    ///     The count is needed for hash verification:
    ///     node_hash = H(kv_hash || left_hash || right_hash || count.to_be_bytes())
    ///
    ///     ─────────────────────────────────────────────────────
    ///
    ///     Chunk Restoration (all tree types):
    ///
    ///     Chunks MUST use KVValueHashFeatureType to preserve:
    ///     - SummedMerkNode(sum)     for SumTree
    ///     - CountedMerkNode(count)  for CountTree
    ///     - BasicMerkNode           for regular trees
    /// ```
    ///
    /// **When used**:
    /// - **Query proofs**: For queried items in ProvableCountTree, where the
    ///   feature_type contains the aggregate count needed for hash
    ///   verification.
    /// - **Chunk restoration**: Required for ALL tree types during sync/restore
    ///   operations, as the feature_type (SummedMerkNode, CountedMerkNode,
    ///   etc.) must be preserved when rebuilding trees from chunks.
    KVValueHashFeatureType(Vec<u8>, Vec<u8>, CryptoHash, TreeFeatureType),

    /// Key, referenced value, and hash of serialized Reference element.
    /// For GroveDB references.
    ///
    /// Contains: `(key, referenced_value, reference_element_hash)`
    ///
    /// ```text
    ///     Query: key "ref_to_X" (a Reference element)
    ///
    ///     Tree A:                    Tree B:
    ///     ┌─────────────┐            ┌─────────────┐
    ///     │ ref_to_X    │───────────►│ X           │
    ///     │ value: path │  resolves  │ value: data │
    ///     │ to Tree B/X │    to      │ "secret"    │
    ///     └─────────────┘            └─────────────┘
    ///
    ///     KVRefValueHash returns:
    ///     - key:                    "ref_to_X"
    ///     - referenced_value:       "secret" (dereferenced value from X)
    ///     - reference_element_hash: H(serialized_reference_element)
    ///                               ▲
    ///                               └── hash of the Reference element bytes,
    ///                                   NOT the referenced value
    ///
    ///     Verification computes:
    ///       combined_value_hash = combine_hash(reference_element_hash, H(referenced_value))
    ///       kv_hash = H(varint(key.len()) || key || combined_value_hash)
    ///
    ///     This matches how References are stored in the merk tree, where:
    ///       node.value_hash = combine_hash(H(ref_bytes), H(referenced_item_bytes))
    /// ```
    ///
    /// **When used**: When a queried element is a GroveDB Reference type. The
    /// `referenced_value` is the resolved value from the referenced location,
    /// while `reference_element_hash` is `H(serialized_reference_element)`.
    /// During verification, these are combined to reconstruct the node's
    /// value_hash, allowing proof verification while returning dereferenced
    /// data.
    KVRefValueHash(Vec<u8>, Vec<u8>, CryptoHash),

    /// Key, value, and count. For queried Items in ProvableCountTree.
    ///
    /// Contains: `(key, value, count)`
    ///
    /// ```text
    ///     Query for an Item in ProvableCountTree:
    ///
    ///            [B] count=5
    ///           /   \
    ///        [A]     [C] ◄── KVCount(key, value, count=2)
    ///      count=2  count=2
    ///         ▲
    ///       Hash
    ///
    ///     Hash computation during verification:
    ///     value_hash = H(value)  ◄── computed from value
    ///     kv_hash = H(varint(key.len()) || key || value_hash)
    ///     node_hash = H(kv_hash || left || right || count.to_be_bytes())
    ///
    ///     For Items, this works correctly because value_hash = H(value).
    ///
    ///     ─────────────────────────────────────────────────────
    ///
    ///     Why current proof generation uses KVValueHashFeatureType:
    ///
    ///     ┌─────────────────────────────────────────────────┐
    ///     │  KVValueHashFeatureType works for ALL elements: │
    ///     │  - Items: value_hash = H(value) ✓               │
    ///     │  - Subtrees: value_hash = combined hash ✓       │
    ///     │                                                 │
    ///     │  KVCount only works for Items, so               │
    ///     │  KVValueHashFeatureType is used uniformly.      │
    ///     └─────────────────────────────────────────────────┘
    /// ```
    ///
    /// **When used**: Valid for queried Items in ProvableCountTree where
    /// value_hash equals `H(value)`. However, current proof generation uses
    /// `KVValueHashFeatureType` uniformly for all element types. The verifier
    /// supports `KVCount` for compatibility.
    KVCount(Vec<u8>, Vec<u8>, u64),

    /// KV hash and count. For non-queried nodes in ProvableCountTree.
    ///
    /// Contains: `(kv_hash, count)`
    ///
    /// ```text
    ///     Query: key "D" in ProvableCountTree
    ///
    ///            [B] ◄── KVHashCount(kv_hash, count=5)
    ///          count=5    (on path, not queried)
    ///           /   \
    ///        [A]     [C]
    ///         ▲     count=2
    ///       Hash       \
    ///                  [D] ◄── queried (KVValueHashFeatureType)
    ///                count=1
    ///
    ///     Node [B] needs count for hash verification:
    ///     node_hash = H(kv_hash || left || right || count.to_be_bytes())
    ///                                               ▲
    ///                            count=5 required ──┘
    ///
    ///     Similar to KVHash but with count for ProvableCountTree.
    /// ```
    ///
    /// **When used**: For nodes in a ProvableCountTree that are ancestors of
    /// queried nodes but are not themselves being queried. Similar to `KVHash`
    /// but includes the aggregate count needed for ProvableCountTree hash
    /// verification.
    KVHashCount(CryptoHash, u64),

    /// Key, referenced value, reference element hash, and feature type.
    /// For queried References in ProvableCountTree.
    ///
    /// Contains: `(key, referenced_value, reference_element_hash, count)`
    ///
    /// ```text
    ///     Query: key "ref_to_X" (a Reference element in ProvableCountTree)
    ///
    ///     ProvableCountTree:             Tree B:
    ///     ┌─────────────┐                ┌─────────────┐
    ///     │ ref_to_X    │───────────────►│ X           │
    ///     │ count=3     │    resolves    │ value: data │
    ///     │ value: path │      to        │ "secret"    │
    ///     └─────────────┘                └─────────────┘
    ///
    ///     KVRefValueHashCount returns:
    ///     - key:                    "ref_to_X"
    ///     - referenced_value:       "secret" (dereferenced value from X)
    ///     - reference_element_hash: H(serialized_reference_element)
    ///     - count:                  3
    ///
    ///     Verification computes:
    ///       combined_value_hash = combine_hash(reference_element_hash,
    ///                                          H(referenced_value))
    ///       kv_hash = H(varint(key.len()) || key || combined_value_hash)
    ///       node_hash = H(kv_hash || left || right || count.to_be_bytes())
    /// ```
    ///
    /// **When used**: When a queried element in a ProvableCountTree is a
    /// Reference type. Like `KVRefValueHash` but includes the count for
    /// node hash verification.
    KVRefValueHashCount(Vec<u8>, Vec<u8>, CryptoHash, u64),

    /// Key, value_hash, and count. For proving absence in ProvableCountTree.
    ///
    /// Contains: `(key, value_hash, count)`
    ///
    /// ```text
    ///     Query: range ["B".."D"] in ProvableCountTree (but C doesn't exist)
    ///
    ///            [B] ◄── KVDigestCount(key, value_hash, count=3)
    ///          count=5    (left boundary)
    ///           /   \
    ///        [A]     [E] ◄── KVDigestCount(key, value_hash, count=2)
    ///      count=2  count=2  (proves no C or D)
    ///         ▲
    ///       Hash
    ///
    ///     Hash computation during verification:
    ///     kv_hash = H(varint(key.len()) || key || value_hash)
    ///     node_hash = H(kv_hash || left || right || count.to_be_bytes())
    ///                                               ▲
    ///                            count required ────┘
    ///
    ///     Similar to KVDigest but with count for ProvableCountTree.
    /// ```
    ///
    /// **When used**: For nodes at query boundaries in ProvableCountTree
    /// (proving absence of keys in a range). The key is needed for range
    /// comparisons, the value is not returned, and the count is needed
    /// for hash verification.
    KVDigestCount(Vec<u8>, CryptoHash, u64),
}

use std::fmt;

#[cfg(any(feature = "minimal", feature = "verify"))]
impl fmt::Display for Node {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let node_string = match self {
            Node::Hash(hash) => format!("Hash(HASH[{}])", hex::encode(hash)),
            Node::KVHash(kv_hash) => format!("KVHash(HASH[{}])", hex::encode(kv_hash)),
            Node::KV(key, value) => {
                format!("KV({}, {})", hex_to_ascii(key), hex_to_ascii(value))
            }
            Node::KVValueHash(key, value, value_hash) => format!(
                "KVValueHash({}, {}, HASH[{}])",
                hex_to_ascii(key),
                hex_to_ascii(value),
                hex::encode(value_hash)
            ),
            Node::KVDigest(key, value_hash) => format!(
                "KVDigest({}, HASH[{}])",
                hex_to_ascii(key),
                hex::encode(value_hash)
            ),
            Node::KVRefValueHash(key, value, value_hash) => format!(
                "KVRefValueHash({}, {}, HASH[{}])",
                hex_to_ascii(key),
                hex_to_ascii(value),
                hex::encode(value_hash)
            ),
            Node::KVValueHashFeatureType(key, value, value_hash, feature_type) => format!(
                "KVValueHashFeatureType({}, {}, HASH[{}], {:?})",
                hex_to_ascii(key),
                hex_to_ascii(value),
                hex::encode(value_hash),
                feature_type
            ),
            Node::KVCount(key, value, count) => format!(
                "KVCount({}, {}, {})",
                hex_to_ascii(key),
                hex_to_ascii(value),
                count
            ),
            Node::KVHashCount(kv_hash, count) => {
                format!("KVHashCount(HASH[{}], {})", hex::encode(kv_hash), count)
            }
            Node::KVRefValueHashCount(key, value, value_hash, count) => format!(
                "KVRefValueHashCount({}, {}, HASH[{}], {})",
                hex_to_ascii(key),
                hex_to_ascii(value),
                hex::encode(value_hash),
                count
            ),
            Node::KVDigestCount(key, value_hash, count) => format!(
                "KVDigestCount({}, HASH[{}], {})",
                hex_to_ascii(key),
                hex::encode(value_hash),
                count
            ),
        };
        write!(f, "{}", node_string)
    }
}

fn hex_to_ascii(hex_value: &[u8]) -> String {
    if hex_value.len() == 1 && hex_value[0] < b"0"[0] {
        hex::encode(hex_value)
    } else {
        String::from_utf8(hex_value.to_vec()).unwrap_or_else(|_| hex::encode(hex_value))
    }
}
