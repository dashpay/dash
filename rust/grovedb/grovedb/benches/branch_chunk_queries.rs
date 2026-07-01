// MIT LICENSE
//
// Copyright (c) 2021 Dash Core Group
//
// Permission is hereby granted, free of charge, to any
// person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the
// Software without restriction, including without
// limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice
// shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

//! Benchmark for GroveDB trunk and branch chunk query functionality.
//!
//! This benchmark creates a ProvableCountSumTree with many elements and tests
//! the iterative query process for finding specific keys using trunk/branch
//! chunk queries via PathTrunkChunkQuery and PathBranchChunkQuery.
//!
//! ## Query Strategy
//!
//! 1. **Trunk Query**: Gets the top N levels of the target tree, returning
//!    elements and leaf keys with their expected hashes.
//!
//! 2. **Branch Queries**: For keys not found in trunk, trace through the BST
//!    structure to find which leaf subtree contains each target key, then query
//!    only those specific branches.
//!
//! This simulates how a client would search for specific keys in a large tree.

use std::{
    collections::{BTreeMap, BTreeSet},
    sync::Arc,
    time::{Duration, Instant},
};

use grovedb::{Element, GroveDb, LeafInfo, PathBranchChunkQuery, PathTrunkChunkQuery};
use grovedb_merk::{
    calculate_chunk_depths_with_minimum, calculate_max_tree_depth_from_count,
    proofs::{encode_into, tree::Tree, Node},
    CryptoHash, TreeFeatureType,
};
use grovedb_version::version::GroveVersion;
use rand::{rngs::SmallRng, Rng, SeedableRng};
use rand_distr::{Distribution, LogNormal};
use tempfile::TempDir;
use tokio::sync::Mutex as TokioMutex;

/// Tracks which leaf key each remaining target key should be queried under.
struct KeyLeafTracker {
    /// For each remaining target key, the leaf key whose subtree contains it
    key_to_leaf: BTreeMap<Vec<u8>, Vec<u8>>,
    /// Refcount for each leaf key (how many remaining keys reference it)
    leaf_refcount: BTreeMap<Vec<u8>, usize>,
    /// LeafInfo (hash + count) for each leaf key
    leaf_info: BTreeMap<Vec<u8>, LeafInfo>,
    /// Source tree for each leaf key (used for ancestor lookups at depth 2+)
    leaf_source_tree: BTreeMap<Vec<u8>, Tree>,
}

impl KeyLeafTracker {
    fn new() -> Self {
        Self {
            key_to_leaf: BTreeMap::new(),
            leaf_refcount: BTreeMap::new(),
            leaf_info: BTreeMap::new(),
            leaf_source_tree: BTreeMap::new(),
        }
    }

    /// Add a target key with its leaf key
    fn add_key(&mut self, target_key: Vec<u8>, leaf_key: Vec<u8>, info: LeafInfo) {
        *self.leaf_refcount.entry(leaf_key.clone()).or_insert(0) += 1;
        self.key_to_leaf.insert(target_key, leaf_key.clone());
        self.leaf_info.insert(leaf_key, info);
    }

    /// Mark a key as found - remove it and decrement refcount
    fn key_found(&mut self, key: &[u8]) {
        if let Some(leaf) = self.key_to_leaf.remove(key) {
            if let Some(count) = self.leaf_refcount.get_mut(&leaf) {
                *count = count.saturating_sub(1);
            }
        }
    }

    /// Update a key's leaf to a new deeper one, with source tree for ancestor
    /// lookups
    fn update_leaf(
        &mut self,
        key: &[u8],
        new_leaf: Vec<u8>,
        new_info: LeafInfo,
        source_tree: Tree,
    ) {
        if let Some(old_leaf) = self.key_to_leaf.get(key).cloned() {
            // Decrement old leaf's refcount
            if let Some(count) = self.leaf_refcount.get_mut(&old_leaf) {
                *count = count.saturating_sub(1);
            }
            // Add to new leaf
            *self.leaf_refcount.entry(new_leaf.clone()).or_insert(0) += 1;
            self.key_to_leaf.insert(key.to_vec(), new_leaf.clone());
            self.leaf_info.insert(new_leaf.clone(), new_info);
            self.leaf_source_tree.insert(new_leaf, source_tree);
        }
    }

    /// Get the source tree for a leaf key (for ancestor lookups)
    fn get_source_tree(&self, leaf_key: &[u8]) -> Option<&Tree> {
        self.leaf_source_tree.get(leaf_key)
    }

    /// Get leaf keys with refcount > 0 (still have targets to find)
    fn active_leaves(&self) -> Vec<(Vec<u8>, LeafInfo)> {
        self.leaf_refcount
            .iter()
            .filter(|(_, &count)| count > 0)
            .filter_map(|(k, _)| self.leaf_info.get(k).map(|info| (k.clone(), *info)))
            .collect()
    }

    /// Get all target keys that map to a specific leaf
    fn keys_for_leaf(&self, leaf: &[u8]) -> Vec<Vec<u8>> {
        self.key_to_leaf
            .iter()
            .filter(|(_, l)| l.as_slice() == leaf)
            .map(|(k, _)| k.clone())
            .collect()
    }

    fn remaining_count(&self) -> usize {
        self.key_to_leaf.len()
    }

    fn is_empty(&self) -> bool {
        self.key_to_leaf.is_empty()
    }
}

/// Helper function to find ancestor of a leaf key using a Tree structure
/// directly. This is used at depth 2+ where we don't have access to
/// GroveTrunkQueryResult.
///
/// Walks up the tree from the leaf until finding a node with count >=
/// min_privacy_tree_count. Never returns the root - stops at one level below
/// root at most.
///
/// Returns (levels_up, ancestor_count, ancestor_key, ancestor_hash) where
/// levels_up is how many levels we went up from the leaf (1 = parent, 2 =
/// grandparent, etc.)
fn get_ancestor_from_tree(
    leaf_key: &[u8],
    min_privacy_tree_count: u64,
    tree: &Tree,
) -> Option<(u8, u64, Vec<u8>, CryptoHash)> {
    use std::cmp::Ordering;

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

    // Collect the path from root to the target key, including Tree refs for count
    // lookup
    fn collect_path<'a>(
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
                    collect_path(target_key, &left.tree, path)
                } else {
                    None
                }
            }
            Ordering::Greater => {
                if let Some(right) = &tree.right {
                    collect_path(target_key, &right.tree, path)
                } else {
                    None
                }
            }
        }
    }

    let mut path = Vec::new();
    collect_path(leaf_key, tree, &mut path)?;

    // path = [root, ..., grandparent, parent, leaf]
    // Walk backwards from leaf (last element) to find first node with count >=
    // min_privacy_tree_count Never return root (index 0), stop at index 1 at
    // most

    let leaf_idx = path.len() - 1;

    // Start from parent (leaf_idx - 1) and go up
    // Min index is 1 (one below root)
    let min_idx = 1;

    for idx in (min_idx..leaf_idx).rev() {
        let (node_tree, ref key, hash) = &path[idx];
        if let Some(count) = get_node_count(node_tree) {
            if count >= min_privacy_tree_count {
                let levels_up = (leaf_idx - idx) as u8;
                return Some((levels_up, count, key.clone(), *hash));
            }
        }
    }

    // If no node had sufficient count, return the node one below root (index 1)
    // unless we're already at or near the root
    if path.len() > 1 {
        let (node_tree, key, hash) = &path[min_idx];
        let levels_up = (leaf_idx - min_idx) as u8;
        let count = get_node_count(node_tree).unwrap_or(0);
        Some((levels_up, count, key.clone(), *hash))
    } else {
        // Path only has root, can't go anywhere
        None
    }
}

/// Extracts the count from a Tree's root node if it's a ProvableCountTree type.
/// This gives the true size of the subtree for privacy calculations.
fn get_tree_root_count(tree: &Tree) -> Option<u64> {
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

/// Traces a key through a tree's BST structure to find which leaf node's
/// subtree would contain it.
///
/// Returns the leaf key and its LeafInfo if the key would be in a truncated
/// subtree, or None if the key doesn't trace to any leaf in this tree.
fn trace_key_in_tree(
    key: &[u8],
    tree: &Tree,
    leaf_keys: &BTreeMap<Vec<u8>, LeafInfo>,
) -> Option<(Vec<u8>, LeafInfo)> {
    use std::cmp::Ordering;

    let node_key = tree.key()?;

    // Check if this node is a leaf key
    if let Some(leaf_info) = leaf_keys.get(node_key) {
        // This node is a leaf - the key would be in this subtree
        return Some((node_key.to_vec(), *leaf_info));
    }

    // Not a leaf, continue BST traversal
    match key.cmp(node_key) {
        Ordering::Equal => None, // Key found at this node (not in a leaf subtree)
        Ordering::Less => {
            // Go left
            if let Some(left) = &tree.left {
                trace_key_in_tree(key, &left.tree, leaf_keys)
            } else {
                None // No left child
            }
        }
        Ordering::Greater => {
            // Go right
            if let Some(right) = &tree.right {
                trace_key_in_tree(key, &right.tree, leaf_keys)
            } else {
                None // No right child
            }
        }
    }
}

/// Privacy metrics - tracks the size of result sets when keys are found
#[derive(Debug, Default)]
struct PrivacyMetrics {
    /// Smallest result set size when a key was found (worst privacy)
    worst_privacy_set_size: usize,
    /// Largest result set size when a key was found (best privacy)
    best_privacy_set_size: usize,
    /// Sum of all set sizes for average calculation
    total_set_sizes: usize,
    /// Number of keys found (for average)
    keys_found_count: usize,
}

impl PrivacyMetrics {
    fn new() -> Self {
        Self {
            worst_privacy_set_size: usize::MAX,
            best_privacy_set_size: 0,
            total_set_sizes: 0,
            keys_found_count: 0,
        }
    }

    fn record_key_found(&mut self, result_set_size: usize) {
        self.worst_privacy_set_size = self.worst_privacy_set_size.min(result_set_size);
        self.best_privacy_set_size = self.best_privacy_set_size.max(result_set_size);
        self.total_set_sizes += result_set_size;
        self.keys_found_count += 1;
    }

    fn worst_privacy(&self) -> f64 {
        if self.worst_privacy_set_size == usize::MAX {
            0.0
        } else {
            1.0 / self.worst_privacy_set_size as f64
        }
    }

    fn best_privacy(&self) -> f64 {
        if self.best_privacy_set_size == 0 {
            0.0
        } else {
            1.0 / self.best_privacy_set_size as f64
        }
    }

    fn average_privacy(&self) -> f64 {
        if self.keys_found_count == 0 {
            0.0
        } else {
            let avg_set_size = self.total_set_sizes as f64 / self.keys_found_count as f64;
            1.0 / avg_set_size
        }
    }
}

/// Metrics for tracking query performance
#[derive(Debug, Default)]
struct QueryMetrics {
    /// Number of queries at each iteration (iteration 0 = trunk, iteration 1+ =
    /// branch rounds)
    queries_by_iteration: Vec<usize>,
    /// Total elements seen across all proofs
    total_elements_seen: usize,
    /// Number of target keys found
    keys_found: usize,
    /// Number of target keys proven absent
    keys_absent: usize,
    /// Total proof generation time
    proof_gen_duration: Duration,
    /// Total proof verification time
    verify_duration: Duration,
    /// Total proof bytes generated
    total_proof_bytes: usize,
}

impl QueryMetrics {
    fn record_query(&mut self, iteration: usize) {
        while self.queries_by_iteration.len() <= iteration {
            self.queries_by_iteration.push(0);
        }
        self.queries_by_iteration[iteration] += 1;
    }

    fn total_queries(&self) -> usize {
        self.queries_by_iteration.iter().sum()
    }

    fn trunk_queries(&self) -> usize {
        self.queries_by_iteration.first().copied().unwrap_or(0)
    }

    fn branch_queries(&self) -> usize {
        self.queries_by_iteration.iter().skip(1).sum()
    }
}

/// Run the branch chunk query benchmark
pub fn run_branch_chunk_query_benchmark() {
    let grove_version = GroveVersion::latest();
    let mut rng = SmallRng::seed_from_u64(12345);

    println!("=== GroveDB Branch Chunk Query Benchmark ===\n");

    // Configuration
    let num_elements = 100_000;
    let batch_size = 10_000;
    let num_batches = num_elements / batch_size;
    let num_existing_keys = 1000;
    let num_nonexistent_keys = 20;
    let max_depth: u8 = 8;
    let min_depth: u8 = 6;
    let min_privacy_tree_count: u64 = 32;

    println!("Configuration:");
    println!("  Elements in tree: {}", num_elements);
    println!("  Existing keys to find: {}", num_existing_keys);
    println!("  Non-existent keys to find: {}", num_nonexistent_keys);
    println!("  Max depth per chunk: {}", max_depth);
    println!("  Min depth per chunk: {}", min_depth);
    println!();

    // Create temporary directory and GroveDb
    let tmp_dir = TempDir::new().expect("failed to create temp dir");
    let db = GroveDb::open(tmp_dir.path()).expect("failed to open grovedb");

    // Create structure: root -> "data" (empty_tree) -> "count_sum_tree"
    // (ProvableCountSumTree)
    println!("Creating GroveDb structure...");

    db.insert::<&[u8], _>(
        &[],
        b"data",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("failed to insert data tree");

    db.insert(
        &[b"data".as_slice()],
        b"count_sum_tree",
        Element::empty_provable_count_sum_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("failed to insert count_sum_tree");

    // Store all keys for later selection
    let mut all_keys: Vec<Vec<u8>> = Vec::with_capacity(num_elements);

    // Insert elements in batches
    println!(
        "Inserting {} elements into ProvableCountSumTree...",
        num_elements
    );

    let path: &[&[u8]] = &[b"data", b"count_sum_tree"];

    for batch_num in 0..num_batches {
        for _ in 0..batch_size {
            // 32-byte random key
            let mut key = [0u8; 32];
            rng.fill(&mut key);

            // Random value
            let value_num: u8 = rng.random_range(1..=20);
            let item_value = vec![value_num];

            // Random sum
            let sum_value: i64 = rng.random_range(1000..=1_000_000);

            db.insert(
                path,
                &key,
                Element::new_item_with_sum_item(item_value, sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("failed to insert item");

            all_keys.push(key.to_vec());
        }

        if (batch_num + 1) % 2 == 0 {
            println!(
                "  Inserted {} elements ({:.1}%)",
                (batch_num + 1) * batch_size,
                ((batch_num + 1) as f64 / num_batches as f64) * 100.0
            );
        }
    }

    println!("Tree created successfully.\n");

    // Select random existing keys to search for
    let mut existing_keys: BTreeSet<Vec<u8>> = BTreeSet::new();
    while existing_keys.len() < num_existing_keys {
        let idx = rng.random_range(0..all_keys.len());
        existing_keys.insert(all_keys[idx].clone());
    }

    // Generate random non-existent keys
    let mut nonexistent_keys: BTreeSet<Vec<u8>> = BTreeSet::new();
    while nonexistent_keys.len() < num_nonexistent_keys {
        let mut key = [0u8; 32];
        rng.fill(&mut key);
        // Make sure it's not in the tree
        if !all_keys.contains(&key.to_vec()) {
            nonexistent_keys.insert(key.to_vec());
        }
    }

    // Combine all target keys
    let target_keys: BTreeSet<Vec<u8>> = existing_keys
        .iter()
        .chain(nonexistent_keys.iter())
        .cloned()
        .collect();

    println!(
        "Searching for {} keys ({} exist, {} don't exist)\n",
        target_keys.len(),
        existing_keys.len(),
        nonexistent_keys.len()
    );

    // Initialize metrics and tracker
    let mut metrics = QueryMetrics::default();
    let mut privacy = PrivacyMetrics::new();
    let mut tracker = KeyLeafTracker::new();

    let tree_path = vec![b"data".to_vec(), b"count_sum_tree".to_vec()];

    println!("Starting iterative search process...\n");

    // === TRUNK QUERY ===
    println!("=== Depth 0: Trunk Query ===");

    let trunk_query =
        PathTrunkChunkQuery::new_with_min_depth(tree_path.clone(), max_depth, min_depth);

    // Generate trunk proof
    let proof_start = Instant::now();
    let trunk_proof = db
        .prove_trunk_chunk(&trunk_query, grove_version)
        .unwrap()
        .expect("failed to generate trunk proof");
    metrics.proof_gen_duration += proof_start.elapsed();
    metrics.record_query(0);
    metrics.total_proof_bytes += trunk_proof.len();

    println!("  Trunk proof size: {} bytes", trunk_proof.len());

    // Verify trunk proof
    let verify_start = Instant::now();
    let (root_hash, trunk_result) =
        GroveDb::verify_trunk_chunk_proof(&trunk_proof, &trunk_query, grove_version)
            .expect("failed to verify trunk proof");
    metrics.verify_duration += verify_start.elapsed();

    println!("  Root hash: {}", hex::encode(&root_hash[..8]));
    println!("  Elements in trunk: {}", trunk_result.elements.len());
    println!("  Leaf keys: {}", trunk_result.leaf_keys.len());
    println!("  Chunk depths: {:?}", trunk_result.chunk_depths);
    println!("  Max tree depth: {}", trunk_result.max_tree_depth);

    // Show count information from leaf_keys
    let counts: Vec<Option<u64>> = trunk_result
        .leaf_keys
        .values()
        .map(|info| info.count)
        .collect();
    let has_counts = counts.iter().filter(|c| c.is_some()).count();
    println!(
        "  Leaf keys with count: {}/{}",
        has_counts,
        trunk_result.leaf_keys.len()
    );
    if has_counts > 0 {
        let total_count: u64 = counts.iter().filter_map(|c| *c).sum();
        let min_count = counts.iter().filter_map(|c| *c).min().unwrap_or(0);
        let max_count = counts.iter().filter_map(|c| *c).max().unwrap_or(0);
        println!(
            "  Count stats: min={}, max={}, total={}",
            min_count, max_count, total_count
        );
        // Print all individual counts (in key order from BTreeMap)
        let all_counts: Vec<u64> = counts.iter().filter_map(|c| *c).collect();
        println!("  All leaf counts (key order): {:?}", all_counts);
    }

    metrics.total_elements_seen += trunk_result.elements.len();

    // Check which target keys are in the trunk and trace others to leaves
    let trunk_set_size = trunk_result.elements.len();
    let mut found_in_trunk = 0;
    let mut absent_in_trunk = 0;
    for target in &target_keys {
        if trunk_result.elements.contains_key(target) {
            metrics.keys_found += 1;
            privacy.record_key_found(trunk_set_size);
            found_in_trunk += 1;
        } else if let Some((leaf_key, leaf_info)) = trunk_result.trace_key_to_leaf(target) {
            tracker.add_key(target.clone(), leaf_key, leaf_info);
        } else {
            // No leaf to query = key proven absent
            metrics.keys_absent += 1;
            absent_in_trunk += 1;
        }
    }

    println!("  Target keys found: {}", found_in_trunk);
    println!("  Keys proven absent: {}", absent_in_trunk);
    println!(
        "  Keys needing branch queries: {}",
        tracker.remaining_count()
    );
    println!(
        "  Active leaf keys to query: {}",
        tracker.active_leaves().len()
    );

    // === ITERATIVE BRANCH QUERIES ===
    let mut iteration = 0usize;

    while !tracker.is_empty() {
        iteration += 1;
        let active_leaves = tracker.active_leaves();

        if active_leaves.is_empty() {
            // No more leaves to query - remaining keys are absent
            let remaining = tracker.remaining_count();
            metrics.keys_absent += remaining;
            println!(
                "\n  No active leaves - {} remaining keys proven absent",
                remaining
            );
            break;
        }

        println!(
            "\n=== Iteration {}: Branch Queries ({} leaves, {} targets remaining) ===",
            iteration,
            active_leaves.len(),
            tracker.remaining_count()
        );

        let mut found_this_round = 0;
        let mut absent_this_round = 0;
        let mut depth_usage: BTreeMap<u8, usize> = BTreeMap::new();
        let mut count_stats: Vec<u64> = Vec::new();
        let mut ancestor_redirects = 0usize;

        // Build query plan: consolidate small leaves to ancestor queries
        // Map: query_key -> (query_hash, query_depth, Vec<original_leaf_keys>)
        let mut query_plan: BTreeMap<Vec<u8>, (CryptoHash, u8, Vec<Vec<u8>>)> = BTreeMap::new();

        for (leaf_key, leaf_info) in &active_leaves {
            let keys_for_this_leaf = tracker.keys_for_leaf(leaf_key);
            if keys_for_this_leaf.is_empty() {
                continue;
            }

            let count = leaf_info.count.expect("expected a count");
            count_stats.push(count);
            let tree_depth = calculate_max_tree_depth_from_count(count);

            println!(
                "leaf count={}, tree_depth={}, min_privacy_tree_count={}, use_ancestor={}",
                count,
                tree_depth,
                min_privacy_tree_count,
                min_privacy_tree_count > count
            );

            // Check if subtree is too small for privacy
            let (query_key, query_hash, query_depth) = if min_privacy_tree_count > count {
                // Try to find ancestor - at depth 1 use trunk_result, at depth 2+ use source
                // tree
                let ancestor = if iteration == 1 {
                    trunk_result.get_ancestor(leaf_key, min_privacy_tree_count)
                } else {
                    // At depth 2+, use the stored source tree for this leaf
                    tracker.get_source_tree(leaf_key).and_then(|source_tree| {
                        // Use get_ancestor helper that works with Tree directly
                        get_ancestor_from_tree(leaf_key, min_privacy_tree_count, source_tree)
                    })
                };

                if let Some((levels_up, ancestor_count, ancestor_key, ancestor_hash)) = ancestor {
                    println!(
                        "found ancestor {} having count {} of {}, going up {}",
                        hex::encode(&ancestor_key),
                        ancestor_count,
                        hex::encode(leaf_key),
                        levels_up
                    );
                    ancestor_redirects += 1;
                    // Query the ancestor with max_depth
                    (ancestor_key, ancestor_hash, max_depth)
                } else {
                    // Couldn't find ancestor, query leaf directly
                    println!(
                        "  [FALLBACK] iteration={}, leaf_key={}, tree_depth={}, count={}",
                        iteration,
                        hex::encode(&leaf_key[..8.min(leaf_key.len())]),
                        tree_depth,
                        count
                    );
                    (leaf_key.clone(), leaf_info.hash, min_depth.max(tree_depth))
                }
            } else {
                // Large enough, query leaf directly
                let chunk_depths =
                    calculate_chunk_depths_with_minimum(tree_depth, max_depth, min_depth);
                println!(
                    "Chunks are {:?} for tree_depth={}, max={}, min={}",
                    chunk_depths, tree_depth, max_depth, min_depth
                );
                if chunk_depths.len() == 1 {
                    (leaf_key.clone(), leaf_info.hash, max_depth)
                } else {
                    (leaf_key.clone(), leaf_info.hash, chunk_depths[0])
                }
            };

            // Add to query plan (consolidate if same ancestor)
            query_plan
                .entry(query_key)
                .or_insert_with(|| (query_hash, query_depth, Vec::new()))
                .2
                .push(leaf_key.clone());
        }

        if ancestor_redirects > 0 {
            println!(
                "  Redirected {} small leaves to ancestor queries ({} unique queries)",
                ancestor_redirects,
                query_plan.len()
            );
        }

        // Execute consolidated queries
        for (query_key, (query_hash, query_depth, leaf_keys_under_query)) in query_plan {
            *depth_usage.entry(query_depth).or_insert(0) += 1;
            println!(
                "Calling PathBranchChunkQuery with {query_depth} at key {}",
                hex::encode(&query_key)
            );
            let branch_query =
                PathBranchChunkQuery::new(tree_path.clone(), query_key.clone(), query_depth);

            // Generate branch proof
            let proof_start = Instant::now();
            let branch_proof_unserialized = db
                .prove_branch_chunk_non_serialized(&branch_query, grove_version)
                .unwrap()
                .expect("failed to generate branch proof");

            // Encode just the proof ops - the verifier will execute them
            let mut branch_proof = Vec::new();
            encode_into(branch_proof_unserialized.proof.iter(), &mut branch_proof);

            metrics.proof_gen_duration += proof_start.elapsed();
            metrics.record_query(iteration);
            metrics.total_proof_bytes += branch_proof.len();

            // Verify branch proof
            let verify_start = Instant::now();
            let branch_result = GroveDb::verify_branch_chunk_proof(
                &branch_proof,
                &branch_query,
                query_hash,
                grove_version,
            )
            .expect("failed to verify branch proof");
            metrics.verify_duration += verify_start.elapsed();

            let branch_set_size = branch_result.elements.len();
            metrics.total_elements_seen += branch_set_size;

            let root_count = get_tree_root_count(&branch_result.tree);
            println!(
                "returned elements={}, root_count={:?}, query_depth={}, leaf keys count={}",
                branch_set_size,
                root_count,
                query_depth,
                branch_result.leaf_keys.len()
            );

            // Process all original leaves that were consolidated into this query
            for original_leaf in leaf_keys_under_query {
                let keys_for_this_leaf = tracker.keys_for_leaf(&original_leaf);

                for target in keys_for_this_leaf {
                    if branch_result.elements.contains_key(&target) {
                        // Found!
                        tracker.key_found(&target);
                        metrics.keys_found += 1;
                        privacy.record_key_found(branch_set_size);
                        found_this_round += 1;
                    } else if let Some((new_leaf, new_info)) =
                        branch_result.trace_key_to_leaf(&target)
                    {
                        // Key is in a deeper subtree - store source tree for ancestor lookups
                        tracker.update_leaf(
                            &target,
                            new_leaf,
                            new_info,
                            branch_result.tree.clone(),
                        );
                    } else {
                        // Key not found and no deeper subtree = absent
                        tracker.key_found(&target);
                        metrics.keys_absent += 1;
                        absent_this_round += 1;
                    }
                }
            }
        }

        println!("  Keys found: {}", found_this_round);
        println!("  Keys proven absent: {}", absent_this_round);
        println!("  Keys remaining: {}", tracker.remaining_count());
        println!(
            "  Active leaves for next round: {}",
            tracker.active_leaves().len()
        );
        if iteration > 1 {
            println!(
                "  Active leaves for next round are: {:?}",
                tracker
                    .active_leaves()
                    .iter()
                    .map(|(key, leaf_info)| format!(
                        "{}: {}",
                        hex::encode(key),
                        leaf_info.count.expect("expected count")
                    ))
                    .collect::<Vec<_>>()
            );
        }
        println!(
            "  Branch depths used: {:?}",
            depth_usage
                .iter()
                .map(|(d, c)| format!("depth {}={}", d, c))
                .collect::<Vec<_>>()
                .join(", ")
        );
        if !count_stats.is_empty() {
            let min_count = count_stats.iter().min().unwrap();
            let max_count = count_stats.iter().max().unwrap();
            let sum: u64 = count_stats.iter().sum();
            println!(
                "  Subtree counts: min={}, max={}, total={}, avg={:.0}",
                min_count,
                max_count,
                sum,
                sum as f64 / count_stats.len() as f64
            );
        }

        // Safety limit
        if iteration > 50 {
            println!("Reached depth limit, stopping.");
            break;
        }
    }

    // Print final metrics
    println!("\n=== Final Results ===");
    println!("Keys found: {}", metrics.keys_found);
    println!("Keys proven absent: {}", metrics.keys_absent);
    println!(
        "Expected: {} found, {} absent",
        num_existing_keys, num_nonexistent_keys
    );

    if metrics.keys_found == num_existing_keys && metrics.keys_absent == num_nonexistent_keys {
        println!("  [OK] All keys accounted for correctly!");
    } else {
        println!("  [WARN] Results don't match expectations");
    }

    let total_queries = metrics.total_queries();
    println!("\n=== Query Metrics ===");
    println!("Total queries: {}", total_queries);
    println!("  Trunk (iteration 0): {}", metrics.trunk_queries());
    println!("  Branch (iteration 1+): {}", metrics.branch_queries());
    println!("\nQueries by iteration:");
    for (iteration, count) in metrics.queries_by_iteration.iter().enumerate() {
        if *count > 0 {
            println!("  Iteration {}: {} queries", iteration, count);
        }
    }
    println!("\nTotal elements seen: {}", metrics.total_elements_seen);

    println!("\n=== Performance Metrics ===");
    println!(
        "Total proof generation time: {:.3}s",
        metrics.proof_gen_duration.as_secs_f64()
    );
    println!(
        "Total verification time: {:.3}s",
        metrics.verify_duration.as_secs_f64()
    );
    println!(
        "Total time: {:.3}s",
        (metrics.proof_gen_duration + metrics.verify_duration).as_secs_f64()
    );
    if total_queries > 0 {
        println!(
            "Average proof gen time per query: {:.3}ms",
            metrics.proof_gen_duration.as_secs_f64() * 1000.0 / total_queries as f64
        );
        println!(
            "Average verify time per query: {:.3}ms",
            metrics.verify_duration.as_secs_f64() * 1000.0 / total_queries as f64
        );
    }

    println!("\n=== Proof Size Metrics ===");
    println!(
        "Total proof bytes: {} ({:.2} KB)",
        metrics.total_proof_bytes,
        metrics.total_proof_bytes as f64 / 1024.0
    );
    if total_queries > 0 {
        println!(
            "Average proof size: {:.0} bytes",
            metrics.total_proof_bytes as f64 / total_queries as f64
        );
    }

    println!("\n=== Efficiency ===");
    let total_target_keys = num_existing_keys + num_nonexistent_keys;
    println!(
        "Queries per target key: {:.2}",
        total_queries as f64 / total_target_keys as f64
    );
    println!(
        "Bytes per target key: {:.1}",
        metrics.total_proof_bytes as f64 / total_target_keys as f64
    );

    println!("\n=== Privacy Metrics ===");
    println!(
        "Worst privacy: 1/{} = {:.6} (smallest result set when key found)",
        privacy.worst_privacy_set_size,
        privacy.worst_privacy()
    );
    println!(
        "Best privacy: 1/{} = {:.6} (largest result set when key found)",
        privacy.best_privacy_set_size,
        privacy.best_privacy()
    );
    println!(
        "Average privacy: {:.6} (avg result set size: {:.1})",
        privacy.average_privacy(),
        if privacy.keys_found_count > 0 {
            privacy.total_set_sizes as f64 / privacy.keys_found_count as f64
        } else {
            0.0
        }
    );
}

/// Derives an HD wallet-style key from an index (deterministic)
fn derive_key_from_index(index: u32) -> Vec<u8> {
    blake3::hash(&index.to_be_bytes()).as_bytes().to_vec()
}

/// Run the branch chunk query benchmark with HD wallet gap limit behavior
///
/// This simulates HD wallet connectivity where:
/// 1. Start searching for keys 0 to gap_limit-1
/// 2. When we find key at index N, extend search to N + gap_limit
/// 3. Dynamically add new keys to the search as we discover used indices
pub fn run_branch_chunk_query_benchmark_with_key_increase() {
    let grove_version = GroveVersion::latest();
    let mut rng = SmallRng::seed_from_u64(12345);

    println!("=== GroveDB HD Wallet Gap Limit Benchmark ===\n");

    // Configuration
    let num_elements = 100_000;
    let batch_size = 10_000;
    let num_batches = num_elements / batch_size;
    let max_used_index: u32 = 500; // Wallet has used indices 0-499
    let gap_limit: u32 = 100;
    let max_depth: u8 = 8;
    let min_depth: u8 = 6;
    let min_privacy_tree_count: u64 = 32;

    println!("Configuration:");
    println!("  Elements in tree: {}", num_elements);
    println!("  Max used index (wallet): {}", max_used_index);
    println!("  Gap limit: {}", gap_limit);
    println!("  Max depth per chunk: {}", max_depth);
    println!("  Min depth per chunk: {}", min_depth);
    println!("  Min privacy tree count: {}", min_privacy_tree_count);
    println!();

    // Create temporary directory and GroveDb
    let tmp_dir = TempDir::new().expect("failed to create temp dir");
    let db = GroveDb::open(tmp_dir.path()).expect("failed to open grovedb");

    // Create structure: root -> "data" (empty_tree) -> "count_sum_tree"
    // (ProvableCountSumTree)
    println!("Creating GroveDb structure...");

    db.insert::<&[u8], _>(
        &[],
        b"data",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("failed to insert data tree");

    db.insert(
        &[b"data".as_slice()],
        b"count_sum_tree",
        Element::empty_provable_count_sum_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("failed to insert count_sum_tree");

    let path: &[&[u8]] = &[b"data", b"count_sum_tree"];

    // Insert wallet keys (indices 0 to max_used_index-1)
    println!(
        "Inserting {} wallet keys (indices 0-{})...",
        max_used_index,
        max_used_index - 1
    );

    // Track which keys are wallet keys (exist in tree)
    let mut wallet_keys: BTreeSet<Vec<u8>> = BTreeSet::new();

    for index in 0..max_used_index {
        let key = derive_key_from_index(index);

        let value_num: u8 = rng.random_range(1..=20);
        let item_value = vec![value_num];
        let sum_value: i64 = rng.random_range(1000..=1_000_000);

        db.insert(
            path,
            &key,
            Element::new_item_with_sum_item(item_value, sum_value),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("failed to insert wallet key");

        wallet_keys.insert(key);
    }

    // Insert noise keys to make the tree large
    let noise_count = num_elements - max_used_index as usize;
    println!("Inserting {} noise keys...", noise_count);

    for batch_num in 0..num_batches {
        let keys_this_batch = batch_size.min(noise_count - batch_num * batch_size);
        if keys_this_batch == 0 {
            break;
        }

        for _ in 0..keys_this_batch {
            // Random 32-byte key (not derived from index)
            let mut key = [0u8; 32];
            rng.fill(&mut key);

            let value_num: u8 = rng.random_range(1..=20);
            let item_value = vec![value_num];
            let sum_value: i64 = rng.random_range(1000..=1_000_000);

            db.insert(
                path,
                &key,
                Element::new_item_with_sum_item(item_value, sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("failed to insert noise key");
        }

        if (batch_num + 1) % 2 == 0 {
            println!(
                "  Inserted {} noise elements ({:.1}%)",
                (batch_num + 1) * batch_size,
                ((batch_num + 1) as f64 / num_batches as f64) * 100.0
            );
        }
    }

    println!("Tree created successfully.\n");

    // HD Wallet state
    let mut highest_found_index: Option<u32> = None;
    let mut current_gap_end: u32 = gap_limit; // Initially search indices 0 to gap_limit-1

    // Track which indices we're currently searching for
    // Map: index -> key
    let mut pending_indices: BTreeMap<u32, Vec<u8>> = BTreeMap::new();
    for index in 0..current_gap_end {
        pending_indices.insert(index, derive_key_from_index(index));
    }

    // Track elements we've already seen (to check new keys against)
    let mut seen_elements: BTreeSet<Vec<u8>> = BTreeSet::new();

    // Store branch trees with their leaf_keys so we can trace new keys through them
    // when gap extension happens and trunk_result.trace_key_to_leaf() returns None
    let mut branch_trees: Vec<(Tree, BTreeMap<Vec<u8>, LeafInfo>)> = Vec::new();

    println!(
        "Starting HD wallet search: indices 0-{} (gap_limit={})\n",
        current_gap_end - 1,
        gap_limit
    );

    // Build initial target keys from pending indices
    let target_keys: BTreeSet<Vec<u8>> = pending_indices.values().cloned().collect();

    // Create reverse lookup: key -> index
    let mut key_to_index: BTreeMap<Vec<u8>, u32> = BTreeMap::new();
    for (index, key) in &pending_indices {
        key_to_index.insert(key.clone(), *index);
    }

    println!(
        "Searching for {} keys (indices 0-{})\n",
        target_keys.len(),
        current_gap_end - 1
    );

    // Initialize metrics and tracker
    let mut metrics = QueryMetrics::default();
    let mut privacy = PrivacyMetrics::new();
    let mut tracker = KeyLeafTracker::new();

    let tree_path = vec![b"data".to_vec(), b"count_sum_tree".to_vec()];

    println!("Starting iterative search process...\n");

    // === TRUNK QUERY ===
    println!("=== Depth 0: Trunk Query ===");

    let trunk_query =
        PathTrunkChunkQuery::new_with_min_depth(tree_path.clone(), max_depth, min_depth);

    // Generate trunk proof
    let proof_start = Instant::now();
    let trunk_proof = db
        .prove_trunk_chunk(&trunk_query, grove_version)
        .unwrap()
        .expect("failed to generate trunk proof");
    metrics.proof_gen_duration += proof_start.elapsed();
    metrics.record_query(0);
    metrics.total_proof_bytes += trunk_proof.len();

    println!("  Trunk proof size: {} bytes", trunk_proof.len());

    // Verify trunk proof
    let verify_start = Instant::now();
    let (root_hash, trunk_result) =
        GroveDb::verify_trunk_chunk_proof(&trunk_proof, &trunk_query, grove_version)
            .expect("failed to verify trunk proof");
    metrics.verify_duration += verify_start.elapsed();

    println!("  Root hash: {}", hex::encode(&root_hash[..8]));
    println!("  Elements in trunk: {}", trunk_result.elements.len());
    println!("  Leaf keys: {}", trunk_result.leaf_keys.len());
    println!("  Chunk depths: {:?}", trunk_result.chunk_depths);
    println!("  Max tree depth: {}", trunk_result.max_tree_depth);

    // Store trunk elements as seen
    for key in trunk_result.elements.keys() {
        seen_elements.insert(key.clone());
    }

    metrics.total_elements_seen += trunk_result.elements.len();

    // Check which target keys are in the trunk and trace others to leaves
    let trunk_set_size = trunk_result.elements.len();
    let mut found_in_trunk = 0;
    let mut absent_in_trunk = 0;

    // Helper closure to extend gap when we find a wallet key
    let mut extend_gap = |key: &[u8],
                          pending: &mut BTreeMap<u32, Vec<u8>>,
                          k2i: &mut BTreeMap<Vec<u8>, u32>,
                          gap_end: &mut u32| {
        if let Some(&index) = k2i.get(key) {
            if wallet_keys.contains(key) {
                // This is a used wallet index - extend the gap
                let new_highest = match highest_found_index {
                    Some(h) if index > h => {
                        highest_found_index = Some(index);
                        index
                    }
                    None => {
                        highest_found_index = Some(index);
                        index
                    }
                    Some(h) => h,
                };

                let new_gap_end = new_highest + gap_limit + 1;
                if new_gap_end > *gap_end {
                    println!(
                        "  [GAP EXTEND] Found index {}, extending search from {} to {}",
                        index,
                        *gap_end - 1,
                        new_gap_end - 1
                    );
                    // Add new indices to pending
                    for new_idx in *gap_end..new_gap_end {
                        let new_key = derive_key_from_index(new_idx);
                        pending.insert(new_idx, new_key.clone());
                        k2i.insert(new_key, new_idx);
                    }
                    *gap_end = new_gap_end;
                }
            }
            // Remove from pending
            pending.remove(&index);
        }
    };

    for target in &target_keys {
        if trunk_result.elements.contains_key(target) {
            metrics.keys_found += 1;
            privacy.record_key_found(trunk_set_size);
            found_in_trunk += 1;
            extend_gap(
                target,
                &mut pending_indices,
                &mut key_to_index,
                &mut current_gap_end,
            );
        } else if let Some((leaf_key, leaf_info)) = trunk_result.trace_key_to_leaf(target) {
            tracker.add_key(target.clone(), leaf_key, leaf_info);
        } else {
            // No leaf to query = key proven absent
            metrics.keys_absent += 1;
            absent_in_trunk += 1;
            // Still remove from pending
            if let Some(&index) = key_to_index.get(target) {
                pending_indices.remove(&index);
            }
        }
    }

    // Check newly added keys against trunk elements and trace to leaves
    // Need to loop because extend_gap can add more keys that also need processing
    let mut new_found_in_trunk = 0;
    let mut processed_keys: BTreeSet<Vec<u8>> = target_keys.clone();

    loop {
        let new_keys: Vec<Vec<u8>> = pending_indices
            .values()
            .filter(|k| !processed_keys.contains(*k))
            .cloned()
            .collect();

        if new_keys.is_empty() {
            break;
        }

        for new_key in &new_keys {
            processed_keys.insert(new_key.clone());

            if trunk_result.elements.contains_key(new_key) {
                metrics.keys_found += 1;
                privacy.record_key_found(trunk_set_size);
                new_found_in_trunk += 1;
                extend_gap(
                    new_key,
                    &mut pending_indices,
                    &mut key_to_index,
                    &mut current_gap_end,
                );
            } else if let Some((leaf_key, leaf_info)) = trunk_result.trace_key_to_leaf(new_key) {
                tracker.add_key(new_key.clone(), leaf_key, leaf_info);
            } else {
                // No leaf = absent
                metrics.keys_absent += 1;
                if let Some(&index) = key_to_index.get(new_key) {
                    pending_indices.remove(&index);
                }
            }
        }
    }

    println!("  Target keys found: {}", found_in_trunk);
    if new_found_in_trunk > 0 {
        println!(
            "  New keys (from gap extension) found in trunk: {}",
            new_found_in_trunk
        );
    }
    println!("  Keys proven absent: {}", absent_in_trunk);
    println!(
        "  Keys needing branch queries: {}",
        tracker.remaining_count()
    );
    println!(
        "  Active leaf keys to query: {}",
        tracker.active_leaves().len()
    );

    // === ITERATIVE BRANCH QUERIES ===
    let mut iteration = 0usize;

    while !tracker.is_empty() {
        iteration += 1;
        let active_leaves = tracker.active_leaves();

        if active_leaves.is_empty() {
            // No more leaves to query - remaining keys are absent
            let remaining = tracker.remaining_count();
            metrics.keys_absent += remaining;
            println!(
                "\n  No active leaves - {} remaining keys proven absent",
                remaining
            );
            break;
        }

        println!(
            "\n=== Iteration {}: Branch Queries ({} leaves, {} targets remaining) ===",
            iteration,
            active_leaves.len(),
            tracker.remaining_count()
        );

        let mut found_this_round = 0;
        let mut absent_this_round = 0;
        let mut depth_usage: BTreeMap<u8, usize> = BTreeMap::new();
        let mut count_stats: Vec<u64> = Vec::new();
        let mut ancestor_redirects = 0usize;

        // Build query plan: consolidate small leaves to ancestor queries
        // Map: query_key -> (query_hash, query_depth, Vec<original_leaf_keys>)
        let mut query_plan: BTreeMap<Vec<u8>, (CryptoHash, u8, Vec<Vec<u8>>)> = BTreeMap::new();

        for (leaf_key, leaf_info) in &active_leaves {
            let keys_for_this_leaf = tracker.keys_for_leaf(leaf_key);
            if keys_for_this_leaf.is_empty() {
                continue;
            }

            let count = leaf_info.count.expect("expected a count");
            count_stats.push(count);
            let tree_depth = calculate_max_tree_depth_from_count(count);

            println!(
                "leaf count={}, tree_depth={}, min_privacy_tree_count={}, use_ancestor={}",
                count,
                tree_depth,
                min_privacy_tree_count,
                min_privacy_tree_count > count
            );

            // Check if subtree is too small for privacy
            let (query_key, query_hash, query_depth) = if min_privacy_tree_count > count {
                // Try to find ancestor - at depth 1 use trunk_result, at depth 2+ use source
                // tree
                let ancestor = if iteration == 1 {
                    trunk_result.get_ancestor(leaf_key, min_privacy_tree_count)
                } else {
                    // At depth 2+, use the stored source tree for this leaf
                    tracker.get_source_tree(leaf_key).and_then(|source_tree| {
                        // Use get_ancestor helper that works with Tree directly
                        get_ancestor_from_tree(leaf_key, min_privacy_tree_count, source_tree)
                    })
                };

                if let Some((levels_up, ancestor_count, ancestor_key, ancestor_hash)) = ancestor {
                    println!(
                        "found ancestor {} having count {} of {}, going up {}",
                        hex::encode(&ancestor_key),
                        ancestor_count,
                        hex::encode(leaf_key),
                        levels_up
                    );
                    ancestor_redirects += 1;
                    // Query the ancestor with max_depth
                    (ancestor_key, ancestor_hash, max_depth)
                } else {
                    // Couldn't find ancestor, query leaf directly
                    println!(
                        "  [FALLBACK] iteration={}, leaf_key={}, tree_depth={}, count={}",
                        iteration,
                        hex::encode(&leaf_key[..8.min(leaf_key.len())]),
                        tree_depth,
                        count
                    );
                    (leaf_key.clone(), leaf_info.hash, min_depth.max(tree_depth))
                }
            } else {
                // Large enough, query leaf directly
                let chunk_depths =
                    calculate_chunk_depths_with_minimum(tree_depth, max_depth, min_depth);
                println!(
                    "Chunks are {:?} for tree_depth={}, max={}, min={}",
                    chunk_depths, tree_depth, max_depth, min_depth
                );
                if chunk_depths.len() == 1 {
                    (leaf_key.clone(), leaf_info.hash, max_depth)
                } else {
                    (leaf_key.clone(), leaf_info.hash, chunk_depths[0])
                }
            };

            // Add to query plan (consolidate if same ancestor)
            query_plan
                .entry(query_key)
                .or_insert_with(|| (query_hash, query_depth, Vec::new()))
                .2
                .push(leaf_key.clone());
        }

        if ancestor_redirects > 0 {
            println!(
                "  Redirected {} small leaves to ancestor queries ({} unique queries)",
                ancestor_redirects,
                query_plan.len()
            );
        }

        // Execute consolidated queries
        for (query_key, (query_hash, query_depth, leaf_keys_under_query)) in query_plan {
            *depth_usage.entry(query_depth).or_insert(0) += 1;
            println!(
                "Calling PathBranchChunkQuery with {query_depth} at key {}",
                hex::encode(&query_key)
            );
            let branch_query =
                PathBranchChunkQuery::new(tree_path.clone(), query_key.clone(), query_depth);

            // Generate branch proof
            let proof_start = Instant::now();
            let branch_proof_unserialized = db
                .prove_branch_chunk_non_serialized(&branch_query, grove_version)
                .unwrap()
                .expect("failed to generate branch proof");

            // Encode just the proof ops - the verifier will execute them
            let mut branch_proof = Vec::new();
            encode_into(branch_proof_unserialized.proof.iter(), &mut branch_proof);

            metrics.proof_gen_duration += proof_start.elapsed();
            metrics.record_query(iteration);
            metrics.total_proof_bytes += branch_proof.len();

            // Verify branch proof
            let verify_start = Instant::now();
            let branch_result = GroveDb::verify_branch_chunk_proof(
                &branch_proof,
                &branch_query,
                query_hash,
                grove_version,
            )
            .expect("failed to verify branch proof");
            metrics.verify_duration += verify_start.elapsed();

            let branch_set_size = branch_result.elements.len();
            metrics.total_elements_seen += branch_set_size;

            let root_count = get_tree_root_count(&branch_result.tree);
            println!(
                "returned elements={}, root_count={:?}, query_depth={}, leaf keys count={}",
                branch_set_size,
                root_count,
                query_depth,
                branch_result.leaf_keys.len()
            );

            // Store branch elements in seen_elements
            for key in branch_result.elements.keys() {
                seen_elements.insert(key.clone());
            }

            // Store this branch tree so we can trace new keys through it during gap
            // extension
            if !branch_result.leaf_keys.is_empty() {
                branch_trees.push((branch_result.tree.clone(), branch_result.leaf_keys.clone()));
            }

            // Process all original leaves that were consolidated into this query
            for original_leaf in leaf_keys_under_query {
                let keys_for_this_leaf = tracker.keys_for_leaf(&original_leaf);

                for target in keys_for_this_leaf {
                    if branch_result.elements.contains_key(&target) {
                        // Found!
                        tracker.key_found(&target);
                        metrics.keys_found += 1;
                        privacy.record_key_found(branch_set_size);
                        found_this_round += 1;

                        // Check if this is a wallet key and extend gap
                        if let Some(&index) = key_to_index.get(&target) {
                            if wallet_keys.contains(&target) {
                                // This is a used wallet index - extend the gap
                                let new_highest = match highest_found_index {
                                    Some(h) if index > h => {
                                        highest_found_index = Some(index);
                                        index
                                    }
                                    None => {
                                        highest_found_index = Some(index);
                                        index
                                    }
                                    Some(h) => h,
                                };

                                let new_gap_end = new_highest + gap_limit + 1;
                                if new_gap_end > current_gap_end {
                                    println!(
                                        "  [GAP EXTEND] Found index {}, extending search from {} \
                                         to {}",
                                        index,
                                        current_gap_end - 1,
                                        new_gap_end - 1
                                    );
                                    // Add new indices to pending
                                    for new_idx in current_gap_end..new_gap_end {
                                        let new_key = derive_key_from_index(new_idx);
                                        pending_indices.insert(new_idx, new_key.clone());
                                        key_to_index.insert(new_key.clone(), new_idx);

                                        // Check if new key is in seen elements
                                        if seen_elements.contains(&new_key) {
                                            // Already found in a previous query
                                            metrics.keys_found += 1;
                                            privacy.record_key_found(branch_set_size);
                                            found_this_round += 1;
                                            pending_indices.remove(&new_idx);
                                        } else {
                                            // Try to trace through trunk first
                                            let traced = trunk_result
                                                .trace_key_to_leaf(&new_key)
                                                .or_else(|| {
                                                    // If trunk doesn't have it, try branch trees
                                                    for (tree, leaf_keys) in &branch_trees {
                                                        if let Some(result) = trace_key_in_tree(
                                                            &new_key, tree, leaf_keys,
                                                        ) {
                                                            return Some(result);
                                                        }
                                                    }
                                                    None
                                                });

                                            if let Some((leaf_key, leaf_info)) = traced {
                                                tracker.add_key(new_key, leaf_key, leaf_info);
                                            } else {
                                                // Key doesn't trace to any leaf - it's in a fully
                                                // queried subtree. Since it's not in seen_elements,
                                                // it doesn't exist (absent).
                                                metrics.keys_absent += 1;
                                                absent_this_round += 1;
                                                pending_indices.remove(&new_idx);
                                            }
                                        }
                                    }
                                    current_gap_end = new_gap_end;
                                }
                            }
                            pending_indices.remove(&index);
                        }
                    } else if let Some((new_leaf, new_info)) =
                        branch_result.trace_key_to_leaf(&target)
                    {
                        // Key is in a deeper subtree - store source tree for ancestor lookups
                        tracker.update_leaf(
                            &target,
                            new_leaf,
                            new_info,
                            branch_result.tree.clone(),
                        );
                    } else {
                        // Key not found and no deeper subtree = absent
                        tracker.key_found(&target);
                        metrics.keys_absent += 1;
                        absent_this_round += 1;
                        // Remove from pending
                        if let Some(&index) = key_to_index.get(&target) {
                            pending_indices.remove(&index);
                        }
                    }
                }
            }
        }

        println!("  Keys found: {}", found_this_round);
        println!("  Keys proven absent: {}", absent_this_round);
        println!("  Keys remaining: {}", tracker.remaining_count());
        println!(
            "  Active leaves for next round: {}",
            tracker.active_leaves().len()
        );
        if iteration > 1 {
            println!(
                "  Active leaves for next round are: {:?}",
                tracker
                    .active_leaves()
                    .iter()
                    .map(|(key, leaf_info)| format!(
                        "{}: {}",
                        hex::encode(key),
                        leaf_info.count.expect("expected count")
                    ))
                    .collect::<Vec<_>>()
            );
        }
        println!(
            "  Branch depths used: {:?}",
            depth_usage
                .iter()
                .map(|(d, c)| format!("depth {}={}", d, c))
                .collect::<Vec<_>>()
                .join(", ")
        );
        if !count_stats.is_empty() {
            let min_count = count_stats.iter().min().unwrap();
            let max_count = count_stats.iter().max().unwrap();
            let sum: u64 = count_stats.iter().sum();
            println!(
                "  Subtree counts: min={}, max={}, total={}, avg={:.0}",
                min_count,
                max_count,
                sum,
                sum as f64 / count_stats.len() as f64
            );
        }

        // Safety limit
        if iteration > 50 {
            println!("Reached depth limit, stopping.");
            break;
        }
    }

    // Diagnostic: Check for unaccounted indices
    println!("\n=== Diagnostic: Accounting Check ===");
    let accounted = metrics.keys_found + metrics.keys_absent;
    let unaccounted_in_pending = pending_indices.len();
    let tracker_remaining = tracker.remaining_count();
    println!(
        "Total indices searched (current_gap_end): {}",
        current_gap_end
    );
    println!("Keys found: {}", metrics.keys_found);
    println!("Keys absent: {}", metrics.keys_absent);
    println!("Total accounted (found + absent): {}", accounted);
    println!("Still in pending_indices: {}", unaccounted_in_pending);
    println!("Still in tracker: {}", tracker_remaining);
    println!(
        "Discrepancy (gap_end - accounted - pending - tracker): {}",
        current_gap_end as i64
            - accounted as i64
            - unaccounted_in_pending as i64
            - tracker_remaining as i64
    );

    if !pending_indices.is_empty() {
        println!(
            "Unaccounted pending indices: {:?}",
            pending_indices.keys().take(20).collect::<Vec<_>>()
        );
    }

    // Print final metrics
    println!("\n=== Final Results ===");
    println!("Keys found: {}", metrics.keys_found);
    println!("Keys proven absent: {}", metrics.keys_absent);
    println!(
        "Highest found index: {:?}, gap ended at: {}",
        highest_found_index, current_gap_end
    );
    println!(
        "Expected: {} wallet keys (indices 0-{})",
        max_used_index,
        max_used_index - 1
    );

    let total_queries = metrics.total_queries();
    println!("\n=== Query Metrics ===");
    println!("Total queries: {}", total_queries);
    println!("  Trunk (iteration 0): {}", metrics.trunk_queries());
    println!("  Branch (iteration 1+): {}", metrics.branch_queries());
    println!("\nQueries by iteration:");
    for (iteration, count) in metrics.queries_by_iteration.iter().enumerate() {
        if *count > 0 {
            println!("  Iteration {}: {} queries", iteration, count);
        }
    }
    println!("\nTotal elements seen: {}", metrics.total_elements_seen);

    println!("\n=== Performance Metrics ===");
    println!(
        "Total proof generation time: {:.3}s",
        metrics.proof_gen_duration.as_secs_f64()
    );
    println!(
        "Total verification time: {:.3}s",
        metrics.verify_duration.as_secs_f64()
    );
    println!(
        "Total time: {:.3}s",
        (metrics.proof_gen_duration + metrics.verify_duration).as_secs_f64()
    );
    if total_queries > 0 {
        println!(
            "Average proof gen time per query: {:.3}ms",
            metrics.proof_gen_duration.as_secs_f64() * 1000.0 / total_queries as f64
        );
        println!(
            "Average verify time per query: {:.3}ms",
            metrics.verify_duration.as_secs_f64() * 1000.0 / total_queries as f64
        );
    }

    println!("\n=== Proof Size Metrics ===");
    println!(
        "Total proof bytes: {} ({:.2} KB)",
        metrics.total_proof_bytes,
        metrics.total_proof_bytes as f64 / 1024.0
    );
    if total_queries > 0 {
        println!(
            "Average proof size: {:.0} bytes",
            metrics.total_proof_bytes as f64 / total_queries as f64
        );
    }

    println!("\n=== HD Wallet Metrics ===");
    let total_searched_indices = current_gap_end;
    println!("Total indices searched: {}", total_searched_indices);
    println!(
        "Queries per index: {:.2}",
        total_queries as f64 / total_searched_indices as f64
    );
    println!(
        "Bytes per index: {:.1}",
        metrics.total_proof_bytes as f64 / total_searched_indices as f64
    );

    println!("\n=== Privacy Metrics ===");
    println!(
        "Worst privacy: 1/{} = {:.6} (smallest result set when key found)",
        privacy.worst_privacy_set_size,
        privacy.worst_privacy()
    );
    println!(
        "Best privacy: 1/{} = {:.6} (largest result set when key found)",
        privacy.best_privacy_set_size,
        privacy.best_privacy()
    );
    println!(
        "Average privacy: {:.6} (avg result set size: {:.1})",
        privacy.average_privacy(),
        if privacy.keys_found_count > 0 {
            privacy.total_set_sizes as f64 / privacy.keys_found_count as f64
        } else {
            0.0
        }
    );
}

/// Generates a realistic network latency in milliseconds.
///
/// Distribution: shifted log-normal with peak around 90ms, min 60ms,
/// rarely over 300ms, max capped at 3000ms.
///
/// Parameters tuned for: mode ≈ 30ms above base (so total mode ≈ 90ms),
/// with a long tail for occasional slow responses.
fn generate_latency(rng: &mut impl Rng) -> Duration {
    const BASE_LATENCY_MS: f64 = 60.0;
    const MAX_LATENCY_MS: f64 = 3000.0;

    // Log-normal parameters: μ=4.0, σ=0.8
    // This gives: mode ≈ exp(4.0 - 0.64) ≈ 29ms, so total mode ≈ 89ms
    // median ≈ exp(4.0) ≈ 55ms, so total median ≈ 115ms
    // 95th percentile ≈ 263ms, 99th percentile ≈ 412ms
    let log_normal = LogNormal::new(4.0, 0.8).unwrap();
    let sample: f64 = log_normal.sample(rng);
    let latency_ms = (BASE_LATENCY_MS + sample).min(MAX_LATENCY_MS);
    Duration::from_millis(latency_ms as u64)
}

/// Latency statistics tracker
#[derive(Debug, Default)]
struct LatencyStats {
    samples: Vec<u64>,
}

impl LatencyStats {
    fn record(&mut self, latency: Duration) {
        self.samples.push(latency.as_millis() as u64);
    }

    fn min(&self) -> u64 {
        *self.samples.iter().min().unwrap_or(&0)
    }

    fn max(&self) -> u64 {
        *self.samples.iter().max().unwrap_or(&0)
    }

    fn mean(&self) -> f64 {
        if self.samples.is_empty() {
            0.0
        } else {
            self.samples.iter().sum::<u64>() as f64 / self.samples.len() as f64
        }
    }

    fn percentile(&self, p: f64) -> u64 {
        if self.samples.is_empty() {
            return 0;
        }
        let mut sorted = self.samples.clone();
        sorted.sort_unstable();
        let idx = ((p / 100.0) * (sorted.len() - 1) as f64).round() as usize;
        sorted[idx.min(sorted.len() - 1)]
    }

    fn count_over(&self, threshold_ms: u64) -> usize {
        self.samples.iter().filter(|&&s| s > threshold_ms).count()
    }
}

/// Run the HD wallet benchmark with simulated async network latency.
///
/// This simulates a real-world client scenario where:
/// 1. Proof requests are sent to remote nodes with network latency
/// 2. Multiple requests can be in-flight concurrently
/// 3. Latency follows a realistic distribution (log-normal, peak ~90ms)
pub fn run_async_latency_benchmark() {
    let rt = tokio::runtime::Runtime::new().unwrap();
    rt.block_on(run_async_latency_benchmark_inner());
}

async fn run_async_latency_benchmark_inner() {
    let grove_version = GroveVersion::latest();
    let mut rng = SmallRng::seed_from_u64(12345);

    println!("=== GroveDB Async Latency Simulation Benchmark ===\n");

    // Configuration
    let num_elements = 1_000_000;
    let batch_size = 100_000;
    let num_batches = num_elements / batch_size;
    let max_used_index: u32 = 500;
    let gap_limit: u32 = 100;
    let max_depth: u8 = 8;
    let min_depth: u8 = 6;
    let min_privacy_tree_count: u64 = 32;
    let max_concurrent_requests: usize = 40; // Simulate querying up to N nodes concurrently

    println!("Configuration:");
    println!("  Elements in tree: {}", num_elements);
    println!("  Max used index (wallet): {}", max_used_index);
    println!("  Gap limit: {}", gap_limit);
    println!("  Max depth per chunk: {}", max_depth);
    println!("  Min depth per chunk: {}", min_depth);
    println!("  Max concurrent requests: {}", max_concurrent_requests);
    println!("  Latency: 60-3000ms (log-normal, peak ~90ms)");
    println!();

    // Create temporary directory and GroveDb
    let tmp_dir = TempDir::new().expect("failed to create temp dir");
    let db = Arc::new(GroveDb::open(tmp_dir.path()).expect("failed to open grovedb"));

    // Create structure
    println!("Creating GroveDb structure...");

    db.insert::<&[u8], _>(
        &[],
        b"data",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("failed to insert data tree");

    db.insert(
        &[b"data".as_slice()],
        b"count_sum_tree",
        Element::empty_provable_count_sum_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("failed to insert count_sum_tree");

    let path: &[&[u8]] = &[b"data", b"count_sum_tree"];

    // Insert wallet keys
    println!(
        "Inserting {} wallet keys (indices 0-{})...",
        max_used_index,
        max_used_index - 1
    );

    let mut wallet_keys: BTreeSet<Vec<u8>> = BTreeSet::new();

    for index in 0..max_used_index {
        let key = derive_key_from_index(index);
        let value_num: u8 = rng.random_range(1..=20);
        let item_value = vec![value_num];
        let sum_value: i64 = rng.random_range(1000..=1_000_000);

        db.insert(
            path,
            &key,
            Element::new_item_with_sum_item(item_value, sum_value),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("failed to insert wallet key");

        wallet_keys.insert(key);
    }

    // Insert noise keys
    let noise_count = num_elements - max_used_index as usize;
    println!("Inserting {} noise keys...", noise_count);

    for batch_num in 0..num_batches {
        let keys_this_batch = batch_size.min(noise_count - batch_num * batch_size);
        if keys_this_batch == 0 {
            break;
        }

        for _ in 0..keys_this_batch {
            let mut key = [0u8; 32];
            rng.fill(&mut key);
            let value_num: u8 = rng.random_range(1..=20);
            let item_value = vec![value_num];
            let sum_value: i64 = rng.random_range(1000..=1_000_000);

            db.insert(
                path,
                &key,
                Element::new_item_with_sum_item(item_value, sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("failed to insert noise key");
        }

        if (batch_num + 1) % 2 == 0 {
            println!(
                "  Inserted {} noise elements ({:.1}%)",
                (batch_num + 1) * batch_size,
                ((batch_num + 1) as f64 / num_batches as f64) * 100.0
            );
        }
    }

    println!("Tree created successfully.\n");

    // HD Wallet state
    let mut highest_found_index: Option<u32> = None;
    let mut current_gap_end: u32 = gap_limit;

    let mut pending_indices: BTreeMap<u32, Vec<u8>> = BTreeMap::new();
    for index in 0..current_gap_end {
        pending_indices.insert(index, derive_key_from_index(index));
    }

    let mut seen_elements: BTreeSet<Vec<u8>> = BTreeSet::new();
    let mut key_to_index: BTreeMap<Vec<u8>, u32> = BTreeMap::new();
    for (index, key) in &pending_indices {
        key_to_index.insert(key.clone(), *index);
    }

    // Metrics
    let mut metrics = QueryMetrics::default();
    let mut privacy = PrivacyMetrics::new();
    let mut tracker = KeyLeafTracker::new();
    let mut latency_stats = LatencyStats::default();
    let mut branch_trees: Vec<(Tree, BTreeMap<Vec<u8>, LeafInfo>)> = Vec::new();

    let tree_path = vec![b"data".to_vec(), b"count_sum_tree".to_vec()];

    // RNG for latency simulation (wrapped in mutex for async use)
    let latency_rng = Arc::new(TokioMutex::new(SmallRng::seed_from_u64(67890)));

    let overall_start = Instant::now();

    println!("Starting async HD wallet search with simulated latency...\n");

    // === TRUNK QUERY (single request) ===
    println!("=== Iteration 0: Trunk Query ===");

    let trunk_query =
        PathTrunkChunkQuery::new_with_min_depth(tree_path.clone(), max_depth, min_depth);

    // Simulate latency for trunk request
    let latency = {
        let mut rng_guard = latency_rng.lock().await;
        generate_latency(&mut *rng_guard)
    };
    println!("  Simulating network latency: {}ms", latency.as_millis());
    tokio::time::sleep(latency).await;
    latency_stats.record(latency);

    let proof_start = Instant::now();
    let trunk_proof = db
        .prove_trunk_chunk(&trunk_query, grove_version)
        .unwrap()
        .expect("failed to generate trunk proof");
    metrics.proof_gen_duration += proof_start.elapsed();
    metrics.record_query(0);
    metrics.total_proof_bytes += trunk_proof.len();

    let verify_start = Instant::now();
    let (root_hash, trunk_result) =
        GroveDb::verify_trunk_chunk_proof(&trunk_proof, &trunk_query, grove_version)
            .expect("failed to verify trunk proof");
    metrics.verify_duration += verify_start.elapsed();

    println!("  Root hash: {}", hex::encode(&root_hash[..8]));
    println!("  Elements in trunk: {}", trunk_result.elements.len());
    println!("  Leaf keys: {}", trunk_result.leaf_keys.len());

    // Store trunk elements
    for key in trunk_result.elements.keys() {
        seen_elements.insert(key.clone());
    }
    metrics.total_elements_seen += trunk_result.elements.len();

    // Process trunk results (similar to sync version but simplified)
    let trunk_set_size = trunk_result.elements.len();
    let target_keys: BTreeSet<Vec<u8>> = pending_indices.values().cloned().collect();

    for target in &target_keys {
        if trunk_result.elements.contains_key(target) {
            metrics.keys_found += 1;
            privacy.record_key_found(trunk_set_size);

            // Check for gap extension
            if let Some(&index) = key_to_index.get(target) {
                if wallet_keys.contains(target) {
                    let new_highest = highest_found_index.map_or(index, |h| h.max(index));
                    highest_found_index = Some(new_highest);
                    let new_gap_end = new_highest + gap_limit + 1;
                    if new_gap_end > current_gap_end {
                        for new_idx in current_gap_end..new_gap_end {
                            let new_key = derive_key_from_index(new_idx);
                            pending_indices.insert(new_idx, new_key.clone());
                            key_to_index.insert(new_key, new_idx);
                        }
                        current_gap_end = new_gap_end;
                    }
                }
                pending_indices.remove(&index);
            }
        } else if let Some((leaf_key, leaf_info)) = trunk_result.trace_key_to_leaf(target) {
            tracker.add_key(target.clone(), leaf_key, leaf_info);
        } else {
            metrics.keys_absent += 1;
            if let Some(&index) = key_to_index.get(target) {
                pending_indices.remove(&index);
            }
        }
    }

    // Process newly added keys from gap extension
    let mut processed_keys: BTreeSet<Vec<u8>> = target_keys.clone();
    loop {
        let new_keys: Vec<Vec<u8>> = pending_indices
            .values()
            .filter(|k| !processed_keys.contains(*k))
            .cloned()
            .collect();

        if new_keys.is_empty() {
            break;
        }

        for new_key in &new_keys {
            processed_keys.insert(new_key.clone());

            if trunk_result.elements.contains_key(new_key) {
                metrics.keys_found += 1;
                privacy.record_key_found(trunk_set_size);

                if let Some(&index) = key_to_index.get(new_key) {
                    if wallet_keys.contains(new_key) {
                        let new_highest = highest_found_index.map_or(index, |h| h.max(index));
                        highest_found_index = Some(new_highest);
                        let new_gap_end = new_highest + gap_limit + 1;
                        if new_gap_end > current_gap_end {
                            for new_idx in current_gap_end..new_gap_end {
                                let nk = derive_key_from_index(new_idx);
                                pending_indices.insert(new_idx, nk.clone());
                                key_to_index.insert(nk, new_idx);
                            }
                            current_gap_end = new_gap_end;
                        }
                    }
                    pending_indices.remove(&index);
                }
            } else if let Some((leaf_key, leaf_info)) = trunk_result.trace_key_to_leaf(new_key) {
                tracker.add_key(new_key.clone(), leaf_key, leaf_info);
            } else {
                metrics.keys_absent += 1;
                if let Some(&index) = key_to_index.get(new_key) {
                    pending_indices.remove(&index);
                }
            }
        }
    }

    println!(
        "  Keys found: {}, Keys needing branch queries: {}",
        metrics.keys_found,
        tracker.remaining_count()
    );

    // === ITERATIVE BRANCH QUERIES WITH CONCURRENT REQUESTS ===
    let mut iteration = 0usize;

    while !tracker.is_empty() {
        iteration += 1;
        let active_leaves = tracker.active_leaves();

        if active_leaves.is_empty() {
            let remaining = tracker.remaining_count();
            metrics.keys_absent += remaining;
            break;
        }

        println!(
            "\n=== Iteration {}: Branch Queries ({} leaves, {} targets remaining) ===",
            iteration,
            active_leaves.len(),
            tracker.remaining_count()
        );

        // Build query plan
        let mut query_plan: Vec<(Vec<u8>, CryptoHash, u8, Vec<Vec<u8>>)> = Vec::new();

        for (leaf_key, leaf_info) in &active_leaves {
            let keys_for_this_leaf = tracker.keys_for_leaf(leaf_key);
            if keys_for_this_leaf.is_empty() {
                continue;
            }

            let count = leaf_info.count.expect("expected a count");
            let tree_depth = calculate_max_tree_depth_from_count(count);

            let (query_key, query_hash, query_depth) = if min_privacy_tree_count > count {
                let ancestor = if iteration == 1 {
                    trunk_result.get_ancestor(leaf_key, min_privacy_tree_count)
                } else {
                    tracker.get_source_tree(leaf_key).and_then(|source_tree| {
                        get_ancestor_from_tree(leaf_key, min_privacy_tree_count, source_tree)
                    })
                };

                if let Some((_, _, ancestor_key, ancestor_hash)) = ancestor {
                    (ancestor_key, ancestor_hash, max_depth)
                } else {
                    (leaf_key.clone(), leaf_info.hash, min_depth.max(tree_depth))
                }
            } else {
                let chunk_depths =
                    calculate_chunk_depths_with_minimum(tree_depth, max_depth, min_depth);
                let depth = if chunk_depths.len() == 1 {
                    max_depth
                } else {
                    chunk_depths[0]
                };
                (leaf_key.clone(), leaf_info.hash, depth)
            };

            // Check if already in query plan
            if let Some(existing) = query_plan.iter_mut().find(|(k, ..)| k == &query_key) {
                existing.3.push(leaf_key.clone());
            } else {
                query_plan.push((query_key, query_hash, query_depth, vec![leaf_key.clone()]));
            }
        }

        // Execute queries in batches with concurrency
        let mut batch_results: Vec<(
            Vec<u8>,
            CryptoHash,
            Vec<Vec<u8>>,
            grovedb::GroveBranchQueryResult,
            Duration,
        )> = Vec::new();

        for chunk in query_plan.chunks(max_concurrent_requests) {
            let mut handles = Vec::new();

            for (query_key, query_hash, query_depth, leaf_keys) in chunk {
                let db_clone = Arc::clone(&db);
                let tree_path_clone = tree_path.clone();
                let query_key_clone = query_key.clone();
                let query_hash_clone = *query_hash;
                let query_depth_clone = *query_depth;
                let leaf_keys_clone = leaf_keys.clone();
                let latency_rng_clone = Arc::clone(&latency_rng);
                let grove_version_clone = grove_version;

                let handle = tokio::spawn(async move {
                    // Simulate network latency
                    let latency = {
                        let mut rng_guard = latency_rng_clone.lock().await;
                        generate_latency(&mut *rng_guard)
                    };
                    tokio::time::sleep(latency).await;

                    let branch_query = PathBranchChunkQuery::new(
                        tree_path_clone,
                        query_key_clone.clone(),
                        query_depth_clone,
                    );

                    let branch_proof_unserialized = db_clone
                        .prove_branch_chunk_non_serialized(&branch_query, grove_version_clone)
                        .unwrap()
                        .expect("failed to generate branch proof");

                    let mut branch_proof = Vec::new();
                    encode_into(branch_proof_unserialized.proof.iter(), &mut branch_proof);

                    let branch_result = GroveDb::verify_branch_chunk_proof(
                        &branch_proof,
                        &branch_query,
                        query_hash_clone,
                        grove_version_clone,
                    )
                    .expect("failed to verify branch proof");

                    (
                        query_key_clone,
                        query_hash_clone,
                        leaf_keys_clone,
                        branch_result,
                        latency,
                        branch_proof.len(),
                    )
                });

                handles.push(handle);
            }

            // Wait for all concurrent requests in this batch
            for handle in handles {
                let (query_key, query_hash, leaf_keys, branch_result, latency, proof_len) =
                    handle.await.unwrap();
                latency_stats.record(latency);
                metrics.record_query(iteration);
                metrics.total_proof_bytes += proof_len;

                batch_results.push((query_key, query_hash, leaf_keys, branch_result, latency));
            }
        }

        // Process results
        let mut found_this_round = 0;
        let mut absent_this_round = 0;

        for (_, _, leaf_keys_under_query, branch_result, _) in batch_results {
            let branch_set_size = branch_result.elements.len();
            metrics.total_elements_seen += branch_set_size;

            // Store seen elements
            for key in branch_result.elements.keys() {
                seen_elements.insert(key.clone());
            }

            // Store branch tree for future tracing
            if !branch_result.leaf_keys.is_empty() {
                branch_trees.push((branch_result.tree.clone(), branch_result.leaf_keys.clone()));
            }

            // Process keys
            for original_leaf in leaf_keys_under_query {
                let keys_for_this_leaf = tracker.keys_for_leaf(&original_leaf);

                for target in keys_for_this_leaf {
                    if branch_result.elements.contains_key(&target) {
                        tracker.key_found(&target);
                        metrics.keys_found += 1;
                        privacy.record_key_found(branch_set_size);
                        found_this_round += 1;

                        // Gap extension
                        if let Some(&index) = key_to_index.get(&target) {
                            if wallet_keys.contains(&target) {
                                let new_highest =
                                    highest_found_index.map_or(index, |h| h.max(index));
                                highest_found_index = Some(new_highest);
                                let new_gap_end = new_highest + gap_limit + 1;
                                if new_gap_end > current_gap_end {
                                    for new_idx in current_gap_end..new_gap_end {
                                        let new_key = derive_key_from_index(new_idx);
                                        pending_indices.insert(new_idx, new_key.clone());
                                        key_to_index.insert(new_key.clone(), new_idx);

                                        if seen_elements.contains(&new_key) {
                                            metrics.keys_found += 1;
                                            privacy.record_key_found(branch_set_size);
                                            found_this_round += 1;
                                            pending_indices.remove(&new_idx);
                                        } else {
                                            let traced = trunk_result
                                                .trace_key_to_leaf(&new_key)
                                                .or_else(|| {
                                                    for (tree, leaf_keys) in &branch_trees {
                                                        if let Some(result) = trace_key_in_tree(
                                                            &new_key, tree, leaf_keys,
                                                        ) {
                                                            return Some(result);
                                                        }
                                                    }
                                                    None
                                                });

                                            if let Some((leaf_key, leaf_info)) = traced {
                                                tracker.add_key(new_key, leaf_key, leaf_info);
                                            } else {
                                                metrics.keys_absent += 1;
                                                absent_this_round += 1;
                                                pending_indices.remove(&new_idx);
                                            }
                                        }
                                    }
                                    current_gap_end = new_gap_end;
                                }
                            }
                            pending_indices.remove(&index);
                        }
                    } else if let Some((new_leaf, new_info)) =
                        branch_result.trace_key_to_leaf(&target)
                    {
                        tracker.update_leaf(
                            &target,
                            new_leaf,
                            new_info,
                            branch_result.tree.clone(),
                        );
                    } else {
                        tracker.key_found(&target);
                        metrics.keys_absent += 1;
                        absent_this_round += 1;
                        if let Some(&index) = key_to_index.get(&target) {
                            pending_indices.remove(&index);
                        }
                    }
                }
            }
        }

        println!(
            "  Keys found: {}, Absent: {}, Remaining: {}",
            found_this_round,
            absent_this_round,
            tracker.remaining_count()
        );

        if iteration > 50 {
            println!("Reached iteration limit, stopping.");
            break;
        }
    }

    let overall_duration = overall_start.elapsed();

    // Print final metrics
    println!("\n=== Final Results ===");
    println!("Keys found: {}", metrics.keys_found);
    println!("Keys proven absent: {}", metrics.keys_absent);
    println!(
        "Highest found index: {:?}, gap ended at: {}",
        highest_found_index, current_gap_end
    );
    println!(
        "Expected: {} wallet keys (indices 0-{})",
        max_used_index,
        max_used_index - 1
    );

    let total_queries = metrics.total_queries();
    println!("\n=== Query Metrics ===");
    println!("Total queries: {}", total_queries);
    println!("  Trunk (iteration 0): {}", metrics.trunk_queries());
    println!("  Branch (iteration 1+): {}", metrics.branch_queries());

    println!("\n=== Latency Statistics ===");
    println!(
        "  Total simulated requests: {}",
        latency_stats.samples.len()
    );
    println!("  Min latency: {}ms", latency_stats.min());
    println!("  Max latency: {}ms", latency_stats.max());
    println!("  Mean latency: {:.1}ms", latency_stats.mean());
    println!("  Median (p50): {}ms", latency_stats.percentile(50.0));
    println!("  p90 latency: {}ms", latency_stats.percentile(90.0));
    println!("  p95 latency: {}ms", latency_stats.percentile(95.0));
    println!("  p99 latency: {}ms", latency_stats.percentile(99.0));
    println!(
        "  Requests over 300ms: {} ({:.1}%)",
        latency_stats.count_over(300),
        if latency_stats.samples.is_empty() {
            0.0
        } else {
            100.0 * latency_stats.count_over(300) as f64 / latency_stats.samples.len() as f64
        }
    );

    println!("\n=== Timing ===");
    println!(
        "Total wall-clock time: {:.2}s",
        overall_duration.as_secs_f64()
    );
    println!(
        "Total simulated network time: {:.2}s",
        latency_stats.samples.iter().sum::<u64>() as f64 / 1000.0
    );
    println!(
        "Actual proof gen time: {:.3}s",
        metrics.proof_gen_duration.as_secs_f64()
    );
    println!(
        "Actual verify time: {:.3}s",
        metrics.verify_duration.as_secs_f64()
    );

    println!("\n=== Proof Size Metrics ===");
    println!(
        "Total proof bytes: {} ({:.2} KB)",
        metrics.total_proof_bytes,
        metrics.total_proof_bytes as f64 / 1024.0
    );

    println!("\n=== Privacy Metrics ===");
    println!(
        "Worst privacy: 1/{} = {:.6}",
        privacy.worst_privacy_set_size,
        privacy.worst_privacy()
    );
    println!(
        "Best privacy: 1/{} = {:.6}",
        privacy.best_privacy_set_size,
        privacy.best_privacy()
    );
    println!(
        "Average privacy: {:.6} (avg result set size: {:.1})",
        privacy.average_privacy(),
        if privacy.keys_found_count > 0 {
            privacy.total_set_sizes as f64 / privacy.keys_found_count as f64
        } else {
            0.0
        }
    );
}

fn main() {
    // Uncomment the one you want to run:
    // run_branch_chunk_query_benchmark();
    // run_branch_chunk_query_benchmark_with_key_increase();
    run_async_latency_benchmark();
}
