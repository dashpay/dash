//! Branch queries for splitting large tree proofs into manageable chunks.
//!
//! This module provides two query types:
//!
//! 1. **Trunk Query** (count-based): Returns top N levels of a count tree, with
//!    optimal depth splitting. Only works on CountTree, CountSumTree, and
//!    ProvableCountTree.
//!
//! 2. **Branch Query** (key-based): Traverses to a key, returns subtree from
//!    that point to specified depth. Works on any tree type.
//!
//! Both return proof structures (verifiable against root hash) with
//! `Node::Hash` for truncated children beyond the specified depth.

pub mod depth;

#[cfg(any(feature = "minimal", feature = "verify"))]
use crate::{
    error::Error,
    proofs::{tree::execute, Node, Op},
    tree::CryptoHash,
};

/// Result from a trunk query operation.
///
/// A trunk query retrieves the top N levels of a count tree, providing
/// enough structure to understand the tree's shape and plan subsequent
/// branch queries.
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TrunkQueryResult {
    /// The proof operations representing the trunk of the tree.
    /// Nodes beyond the first chunk depth are replaced with `Node::Hash`.
    pub proof: Vec<Op>,

    /// Calculated chunk depths for optimal splitting.
    /// For example, tree_depth=20 with max_depth=8 yields `[7, 7, 6]`
    /// instead of naive `[8, 8, 4]`.
    pub chunk_depths: Vec<u8>,

    /// The calculated total depth of the tree based on element count.
    pub tree_depth: u8,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl TrunkQueryResult {
    /// Returns the keys of trunk leaf nodes (nodes whose children are
    /// `Node::Hash`).
    ///
    /// These are the keys at the boundary of the trunk - one level above the
    /// truncated subtrees. These keys can be used as entry points for branch
    /// queries to explore deeper levels of the tree.
    ///
    /// # Returns
    ///
    /// A vector of keys for nodes that have `Node::Hash` children.
    pub fn terminal_node_keys(&self) -> Vec<Vec<u8>> {
        // Execute the proof to build the tree structure
        let tree = match execute(self.proof.iter().map(|op| Ok(op.clone())), false, |_node| {
            Ok(())
        })
        .unwrap()
        {
            Ok(tree) => tree,
            Err(_) => return Vec::new(),
        };

        // Collect keys of nodes that have Hash children
        let mut terminal_keys = Vec::new();
        Self::collect_terminal_keys(&tree, &mut terminal_keys);
        terminal_keys
    }

    /// Recursively collect keys of nodes that have Node::Hash children.
    fn collect_terminal_keys(tree: &crate::proofs::tree::Tree, keys: &mut Vec<Vec<u8>>) {
        // Check if this node has any Hash children
        let has_hash_child = tree
            .left
            .as_ref()
            .map(|c| matches!(c.tree.node, Node::Hash(_)))
            .unwrap_or(false)
            || tree
                .right
                .as_ref()
                .map(|c| matches!(c.tree.node, Node::Hash(_)))
                .unwrap_or(false);

        if has_hash_child {
            // Extract key from this node
            if let Some(key) = Self::get_key_from_node(&tree.node) {
                keys.push(key);
            }
        }

        // Recurse into non-Hash children
        if let Some(left) = &tree.left {
            if !matches!(left.tree.node, Node::Hash(_)) {
                Self::collect_terminal_keys(&left.tree, keys);
            }
        }
        if let Some(right) = &tree.right {
            if !matches!(right.tree.node, Node::Hash(_)) {
                Self::collect_terminal_keys(&right.tree, keys);
            }
        }
    }

    /// Extract key from a node if it has one.
    fn get_key_from_node(node: &Node) -> Option<Vec<u8>> {
        match node {
            Node::KV(key, _)
            | Node::KVValueHash(key, ..)
            | Node::KVValueHashFeatureType(key, ..)
            | Node::KVDigest(key, _)
            | Node::KVDigestCount(key, ..)
            | Node::KVRefValueHash(key, ..)
            | Node::KVCount(key, ..)
            | Node::KVRefValueHashCount(key, ..) => Some(key.clone()),
            Node::Hash(_) | Node::KVHash(_) | Node::KVHashCount(..) => None,
        }
    }

    /// Traces a target key through the proof's BST structure to find which
    /// terminal node (node with Hash children) the key would be under.
    ///
    /// # Arguments
    /// * `target_key` - The key to trace through the tree
    ///
    /// # Returns
    /// * `Some(terminal_key)` - If the key should be in a terminal node's Hash
    ///   subtree
    /// * `None` - If the key was found in the proof (not under a terminal) or
    ///   doesn't exist in the tree
    pub fn trace_key_to_terminal(&self, target_key: &[u8]) -> Option<Vec<u8>> {
        // Execute the proof to build the tree structure
        let tree = match execute(self.proof.iter().map(|op| Ok(op.clone())), false, |_node| {
            Ok(())
        })
        .unwrap()
        {
            Ok(tree) => tree,
            Err(_) => return None,
        };

        Self::trace_key_in_tree(&tree, target_key)
    }

    /// Recursively trace a key through the proof tree to find its terminal
    /// node.
    fn trace_key_in_tree(tree: &crate::proofs::tree::Tree, target_key: &[u8]) -> Option<Vec<u8>> {
        use std::cmp::Ordering;

        // Get the current node's key
        let current_key = match Self::get_key_from_node(&tree.node) {
            Some(k) => k,
            None => {
                // This is a Hash node - shouldn't happen at the root of a valid proof
                return None;
            }
        };

        match target_key.cmp(&current_key) {
            Ordering::Equal => {
                // Found the key in the proof - it's not under a terminal
                None
            }
            Ordering::Less => {
                // Key is smaller, should go left
                match &tree.left {
                    Some(left_child) if !matches!(left_child.tree.node, Node::Hash(_)) => {
                        // Left child is a real node, continue tracing
                        Self::trace_key_in_tree(&left_child.tree, target_key)
                    }
                    Some(_) => {
                        // Left child is a Hash node - target is in this terminal's left subtree
                        Some(current_key)
                    }
                    None => {
                        // No left child - key doesn't exist in tree
                        None
                    }
                }
            }
            Ordering::Greater => {
                // Key is larger, should go right
                match &tree.right {
                    Some(right_child) if !matches!(right_child.tree.node, Node::Hash(_)) => {
                        // Right child is a real node, continue tracing
                        Self::trace_key_in_tree(&right_child.tree, target_key)
                    }
                    Some(_) => {
                        // Right child is a Hash node - target is in this terminal's right subtree
                        Some(current_key)
                    }
                    None => {
                        // No right child - key doesn't exist in tree
                        None
                    }
                }
            }
        }
    }

    /// Verifies that all `Node::Hash` entries are at the expected terminal
    /// depth.
    ///
    /// This validates that all terminal nodes (truncated subtrees) are at the
    /// boundary of the trunk, which is the first chunk depth. This ensures the
    /// trunk was correctly generated with consistent depth limiting.
    ///
    /// # Returns
    ///
    /// * `Ok(())` if all Hash nodes are at the expected depth
    /// * `Err(Error)` if verification fails (wrong depth, invalid proof, etc.)
    pub fn verify_terminal_nodes_at_expected_depth(&self) -> Result<(), Error> {
        let expected_depth = self.chunk_depths.first().copied().unwrap_or(0) as usize;

        // Execute the proof to build the tree structure
        let tree = execute(self.proof.iter().map(|op| Ok(op.clone())), false, |_node| {
            Ok(())
        })
        .unwrap()
        .map_err(|e| Error::InvalidProofError(format!("Failed to execute proof: {}", e)))?;

        // Walk the tree and collect depths of all Node::Hash entries
        let mut hash_depths = Vec::new();
        Self::collect_hash_depths(&tree, 0, &mut hash_depths);

        // Verify all Hash nodes are at the expected depth
        for (hash, depth) in &hash_depths {
            if *depth != expected_depth {
                return Err(Error::InvalidProofError(format!(
                    "Terminal Node::Hash at depth {} (expected {}), hash: {}",
                    depth,
                    expected_depth,
                    hex::encode(hash)
                )));
            }
        }

        Ok(())
    }

    /// Recursively collect depths of all Node::Hash entries in the tree.
    fn collect_hash_depths(
        tree: &crate::proofs::tree::Tree,
        current_depth: usize,
        hash_depths: &mut Vec<(CryptoHash, usize)>,
    ) {
        // Check if this node is a Hash
        if let Node::Hash(hash) = &tree.node {
            hash_depths.push((*hash, current_depth));
        }

        // Recurse into children
        if let Some(left) = &tree.left {
            Self::collect_hash_depths(&left.tree, current_depth + 1, hash_depths);
        }
        if let Some(right) = &tree.right {
            Self::collect_hash_depths(&right.tree, current_depth + 1, hash_depths);
        }
    }
}

/// Result from a branch query operation.
///
/// A branch query navigates to a specific key in the tree and returns
/// the subtree rooted at that key, up to a specified depth.
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BranchQueryResult {
    /// The proof operations representing the branch subtree.
    /// Nodes beyond the specified depth are replaced with `Node::Hash`.
    pub proof: Vec<Op>,

    /// The key at the root of the returned branch.
    pub branch_root_key: Vec<u8>,

    /// The depth of the returned subtree.
    pub returned_depth: u8,

    /// The hash of the branch root node, which should match a `Node::Hash`
    /// entry in the parent trunk proof for verification.
    pub branch_root_hash: CryptoHash,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl BranchQueryResult {
    /// Traces a target key through the proof's BST structure to find which
    /// terminal node (node with Hash children) the key would be under.
    ///
    /// # Arguments
    /// * `target_key` - The key to trace through the tree
    ///
    /// # Returns
    /// * `Some(terminal_key)` - If the key should be in a terminal node's Hash
    ///   subtree
    /// * `None` - If the key was found in the proof (not under a terminal) or
    ///   doesn't exist in the tree
    pub fn trace_key_to_terminal(&self, target_key: &[u8]) -> Option<Vec<u8>> {
        // Execute the proof to build the tree structure
        let tree = match execute(self.proof.iter().map(|op| Ok(op.clone())), false, |_node| {
            Ok(())
        })
        .unwrap()
        {
            Ok(tree) => tree,
            Err(_) => return None,
        };

        Self::trace_key_in_tree(&tree, target_key)
    }

    /// Recursively trace a key through the proof tree to find its terminal
    /// node.
    fn trace_key_in_tree(tree: &crate::proofs::tree::Tree, target_key: &[u8]) -> Option<Vec<u8>> {
        use std::cmp::Ordering;

        // Get the current node's key
        let current_key = match Self::get_key_from_node(&tree.node) {
            Some(k) => k,
            None => {
                // This is a Hash node - shouldn't happen at the root of a valid proof
                return None;
            }
        };

        match target_key.cmp(&current_key) {
            Ordering::Equal => {
                // Found the key in the proof - it's not under a terminal
                None
            }
            Ordering::Less => {
                // Key is smaller, should go left
                match &tree.left {
                    Some(left_child) if !matches!(left_child.tree.node, Node::Hash(_)) => {
                        // Left child is a real node, continue tracing
                        Self::trace_key_in_tree(&left_child.tree, target_key)
                    }
                    Some(_) => {
                        // Left child is a Hash node - target is in this terminal's left subtree
                        Some(current_key)
                    }
                    None => {
                        // No left child - key doesn't exist in tree
                        None
                    }
                }
            }
            Ordering::Greater => {
                // Key is larger, should go right
                match &tree.right {
                    Some(right_child) if !matches!(right_child.tree.node, Node::Hash(_)) => {
                        // Right child is a real node, continue tracing
                        Self::trace_key_in_tree(&right_child.tree, target_key)
                    }
                    Some(_) => {
                        // Right child is a Hash node - target is in this terminal's right subtree
                        Some(current_key)
                    }
                    None => {
                        // No right child - key doesn't exist in tree
                        None
                    }
                }
            }
        }
    }

    /// Extract key from a node if it has one.
    fn get_key_from_node(node: &Node) -> Option<Vec<u8>> {
        match node {
            Node::KV(key, _)
            | Node::KVValueHash(key, ..)
            | Node::KVValueHashFeatureType(key, ..)
            | Node::KVDigest(key, _)
            | Node::KVDigestCount(key, ..)
            | Node::KVRefValueHash(key, ..)
            | Node::KVCount(key, ..)
            | Node::KVRefValueHashCount(key, ..) => Some(key.clone()),
            Node::Hash(_) | Node::KVHash(_) | Node::KVHashCount(..) => None,
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
pub use depth::{
    calculate_chunk_depths, calculate_chunk_depths_with_minimum,
    calculate_max_tree_depth_from_count,
};
