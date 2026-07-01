//! Grove trunk query result for verified trunk chunk proofs.
//!
//! Contains the verified elements from a trunk query as GroveDB Elements,
//! along with leaf keys and their hashes for subsequent branch queries.

use std::{cmp::Ordering, collections::BTreeMap};

use grovedb_merk::{
    proofs::{tree::Tree, Node},
    CryptoHash, TreeFeatureType,
};

use crate::Element;

/// Information about a leaf node for branch queries.
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LeafInfo {
    /// The hash of the node, used to verify branch proofs.
    pub hash: CryptoHash,
    /// The count of elements in this subtree (if available from aggregate
    /// data). Only present for ProvableCountSumTree, CountTree,
    /// CountSumTree.
    pub count: Option<u64>,
}

/// Result from verifying a trunk chunk proof at the GroveDB level.
///
/// Unlike `TrunkQueryResult` which contains raw proof ops, this struct
/// contains deserialized GroveDB Elements and provides the leaf keys
/// needed for subsequent branch queries.
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone)]
pub struct GroveTrunkQueryResult {
    /// The elements from the trunk proof, keyed by their key.
    /// These are the deserialized GroveDB Elements from the proof nodes.
    pub elements: BTreeMap<Vec<u8>, Element>,

    /// Leaf nodes (nodes whose children are `Node::Hash` placeholders).
    /// Maps key -> LeafInfo containing hash and optional count.
    /// The hash is the hash of the node at that key, which should match
    /// the branch_root_hash when verifying a branch proof for that key.
    /// The count (if available) indicates how many elements are in that
    /// subtree. Will be empty if the entire subtree was returned.
    pub leaf_keys: BTreeMap<Vec<u8>, LeafInfo>,

    /// Calculated chunk depths for optimal splitting.
    /// For example, tree_depth=20 with max_depth=8 yields `[7, 7, 6]`
    /// instead of naive `[8, 8, 4]`.
    pub chunk_depths: Vec<u8>,

    /// The calculated total depth of the tree based on element count.
    pub max_tree_depth: u8,

    /// The reconstructed tree structure from the proof.
    /// Used for tracing keys to their terminal (leaf) nodes.
    pub tree: Tree,
}

impl GroveTrunkQueryResult {
    /// Traces a key through the BST structure to find which leaf node's
    /// subtree would contain it.
    ///
    /// Returns the leaf key and its LeafInfo (hash + count) if the key would
    /// be in a truncated subtree, or None if the key is already in the trunk
    /// elements or doesn't exist in any leaf subtree.
    pub fn trace_key_to_leaf(&self, key: &[u8]) -> Option<(Vec<u8>, LeafInfo)> {
        // If key is already in elements, no need to trace
        if self.elements.contains_key(key) {
            return None;
        }

        Self::trace_key_in_tree(key, &self.tree, &self.leaf_keys)
    }

    /// Finds an ancestor of a leaf key with sufficient count for privacy.
    ///
    /// Walks up the tree from the leaf until finding a node with count >=
    /// min_privacy_tree_count. Never returns the root - stops at one level
    /// below root at most.
    ///
    /// # Arguments
    /// * `leaf_key` - The key of the leaf node
    /// * `min_privacy_tree_count` - Minimum count required for privacy
    ///
    /// # Returns
    /// * `Some((levels_up, ancestor_count, ancestor_key, ancestor_hash))` - How
    ///   many levels up, count, and the ancestor's key/hash
    /// * `None` - If the leaf key isn't found or path is too short
    pub fn get_ancestor(
        &self,
        leaf_key: &[u8],
        min_privacy_tree_count: u64,
    ) -> Option<(u8, u64, Vec<u8>, CryptoHash)> {
        // Collect the path from root to leaf, including Tree refs for count lookup
        let mut path = Vec::new();
        Self::collect_path_to_key_with_tree(leaf_key, &self.tree, &mut path)?;

        // path = [root, ..., grandparent, parent, leaf]
        // Walk backwards from leaf to find first node with count >=
        // min_privacy_tree_count Never return root (index 0), stop at index 1
        // at most

        let leaf_idx = path.len() - 1;

        // Start from parent (leaf_idx - 1) and go up
        // Min index is 1 (one below root)
        let min_idx = 1;

        for idx in (min_idx..leaf_idx).rev() {
            let (node_tree, ref key, hash) = &path[idx];
            if let Some(count) = Self::get_node_count(node_tree) {
                if count >= min_privacy_tree_count {
                    let levels_up = (leaf_idx - idx) as u8;
                    return Some((levels_up, count, key.clone(), *hash));
                }
            }
        }

        // If no node had sufficient count, return the node one below root (index 1)
        // but only if that node is strictly above the leaf (not the leaf itself)
        if path.len() > 1 && min_idx < leaf_idx {
            let (node_tree, key, hash) = &path[min_idx];
            let levels_up = (leaf_idx - min_idx) as u8;
            let count = Self::get_node_count(node_tree).unwrap_or(0);
            Some((levels_up, count, key.clone(), *hash))
        } else {
            // Path only has root, or leaf is a direct child of root (no valid ancestor)
            None
        }
    }

    /// Get count from a tree node
    fn get_node_count(tree: &Tree) -> Option<u64> {
        match &tree.node {
            Node::KVCount(_, _, count) => Some(*count),
            Node::KVValueHashFeatureType(_, _, _, feature_type) => match feature_type {
                TreeFeatureType::ProvableCountedMerkNode(count) => Some(*count),
                TreeFeatureType::ProvableCountedSummedMerkNode(count, _) => Some(*count),
                _ => None,
            },
            _ => None,
        }
    }

    /// Collects the path from root to a target key, storing (Tree, key, hash)
    /// tuples.
    fn collect_path_to_key_with_tree<'a>(
        target_key: &[u8],
        tree: &'a Tree,
        path: &mut Vec<(&'a Tree, Vec<u8>, CryptoHash)>,
    ) -> Option<()> {
        let node_key = tree.key()?;
        let node_hash = tree.hash().unwrap();

        // Add this node to path
        path.push((tree, node_key.to_vec(), node_hash));

        match target_key.cmp(node_key) {
            Ordering::Equal => Some(()), // Found it
            Ordering::Less => {
                if let Some(left) = &tree.left {
                    Self::collect_path_to_key_with_tree(target_key, &left.tree, path)
                } else {
                    None
                }
            }
            Ordering::Greater => {
                if let Some(right) = &tree.right {
                    Self::collect_path_to_key_with_tree(target_key, &right.tree, path)
                } else {
                    None
                }
            }
        }
    }

    fn trace_key_in_tree(
        key: &[u8],
        tree: &Tree,
        leaf_keys: &BTreeMap<Vec<u8>, LeafInfo>,
    ) -> Option<(Vec<u8>, LeafInfo)> {
        let node_key = tree.key()?;

        // Check if this node is a leaf key
        if let Some(leaf_info) = leaf_keys.get(node_key) {
            // This node is a leaf - check if the key would be in this subtree
            // For a leaf node with key K:
            // - Keys < K would be in left subtree
            // - Keys > K would be in right subtree
            // Since both subtrees are truncated for a leaf, the key is "here"
            return Some((node_key.to_vec(), *leaf_info));
        }

        // Not a leaf, continue BST traversal
        match key.cmp(node_key) {
            Ordering::Equal => None, // Key found at this node (shouldn't happen since we
            // checked elements)
            Ordering::Less => {
                // Go left
                if let Some(left) = &tree.left {
                    Self::trace_key_in_tree(key, &left.tree, leaf_keys)
                } else {
                    None // No left child, key doesn't exist
                }
            }
            Ordering::Greater => {
                // Go right
                if let Some(right) = &tree.right {
                    Self::trace_key_in_tree(key, &right.tree, leaf_keys)
                } else {
                    None // No right child, key doesn't exist
                }
            }
        }
    }
}
