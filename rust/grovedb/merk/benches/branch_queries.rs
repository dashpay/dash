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

//! Benchmark for trunk and branch query functionality.
//!
//! This benchmark creates a CountSumTree with 1 million elements and tests
//! the iterative query process for finding specific keys using trunk/branch
//! queries.
//!
//! ## Query Strategy
//!
//! The benchmark demonstrates two approaches:
//!
//! 1. **Trunk Query**: Gets the top N levels of the tree (e.g., depth 7 for
//!    ~127 keys). The trunk proof includes `Node::Hash` entries for truncated
//!    subtrees below.
//!
//! 2. **Branch Queries**: For keys not found in trunk, query deeper subtrees.
//!
//! ### Current Implementation
//!
//! The current approach queries target keys directly using
//! `branch_query(target)`, which navigates to the target and returns its
//! subtree. Each proof is checked against ALL remaining targets to find "bonus"
//! matches when proofs overlap.
//!
//! ### Optimal Implementation (Future Enhancement)
//!
//! A more efficient approach would be to query trunk leaf keys (keys with Hash
//! children) instead of individual targets:
//!
//! 1. Parse trunk proof to reconstruct BST structure
//! 2. For each target, trace BST path to find which trunk leaf's subtree
//!    contains it
//! 3. Group targets by trunk leaf
//! 4. Query each trunk leaf ONCE, checking all grouped targets against the
//!    proof
//!
//! This would reduce queries when multiple targets fall under the same trunk
//! leaf. With 127 trunk leaves and 1000 targets, optimal grouping could
//! potentially reduce branch queries from ~1000 to ~127.

use std::{
    collections::{BTreeMap, BTreeSet},
    time::{Duration, Instant},
};

use grovedb_costs::{CostsExt, OperationCost};
use grovedb_element::Element;
use grovedb_merk::{
    proofs::{Node, Op},
    test_utils::TempMerk,
    tree_type::TreeType,
    TreeFeatureType,
};
use grovedb_version::version::GroveVersion;
use rand::{rngs::SmallRng, Rng, SeedableRng};

/// Tracks which terminal key each remaining key should be queried under.
/// Uses BST path tracing through proof structure, not value-based boundaries.
struct KeyTerminalTracker {
    /// For each remaining key, the terminal key whose subtree contains it
    key_to_terminal: BTreeMap<Vec<u8>, Vec<u8>>,
    /// Refcount for each terminal key (how many remaining keys reference it)
    terminal_refcount: BTreeMap<Vec<u8>, usize>,
}

impl KeyTerminalTracker {
    fn new() -> Self {
        Self {
            key_to_terminal: BTreeMap::new(),
            terminal_refcount: BTreeMap::new(),
        }
    }

    /// Add a remaining key with its target terminal key
    fn add_key(&mut self, key: Vec<u8>, terminal: Vec<u8>) {
        *self.terminal_refcount.entry(terminal.clone()).or_insert(0) += 1;
        self.key_to_terminal.insert(key, terminal);
    }

    /// Key was found - remove it and decrement its terminal's refcount
    fn key_found(&mut self, key: &[u8]) {
        if let Some(terminal) = self.key_to_terminal.remove(key) {
            self.decrement(&terminal);
        }
    }

    /// Update a key's terminal to a new one (for deeper levels)
    fn update_terminal(&mut self, key: &[u8], new_terminal: Vec<u8>) {
        if let Some(old_terminal) = self.key_to_terminal.get(key).cloned() {
            self.decrement(&old_terminal);
            *self
                .terminal_refcount
                .entry(new_terminal.clone())
                .or_insert(0) += 1;
            self.key_to_terminal.insert(key.to_vec(), new_terminal);
        }
    }

    fn decrement(&mut self, terminal: &[u8]) {
        if let Some(count) = self.terminal_refcount.get_mut(terminal) {
            *count = count.saturating_sub(1);
        }
    }

    /// Get terminal keys with refcount > 0
    fn active_terminal_keys(&self) -> Vec<Vec<u8>> {
        self.terminal_refcount
            .iter()
            .filter(|(_, &count)| count > 0)
            .map(|(k, _)| k.clone())
            .collect()
    }

    /// Get all keys that map to a specific terminal
    fn keys_for_terminal(&self, terminal: &[u8]) -> Vec<Vec<u8>> {
        self.key_to_terminal
            .iter()
            .filter(|(_, t)| t.as_slice() == terminal)
            .map(|(k, _)| k.clone())
            .collect()
    }

    fn remaining_count(&self) -> usize {
        self.key_to_terminal.len()
    }

    fn is_empty(&self) -> bool {
        self.key_to_terminal.is_empty()
    }
}

// BST tracing is now done via TrunkQueryResult::trace_key_to_terminal() and
// BranchQueryResult::trace_key_to_terminal() which have access to internal
// execute()

/// Metrics for tracking query performance
#[derive(Debug)]
struct QueryMetrics {
    /// Number of queries performed (trunk + branch)
    total_queries: usize,
    /// Total nodes processed across all proofs
    total_nodes_processed: usize,
    /// Number of keys found
    keys_found: usize,
    /// Number of keys proven absent
    keys_absent: usize,
    /// Total bytes loaded from storage across all queries
    storage_loaded_bytes: u64,
    /// Total storage seek operations
    seek_count: u32,
    /// Total hash node calls
    hash_node_calls: u32,
    /// Total time spent executing queries
    query_duration: Duration,
}

impl Default for QueryMetrics {
    fn default() -> Self {
        Self {
            total_queries: 0,
            total_nodes_processed: 0,
            keys_found: 0,
            keys_absent: 0,
            storage_loaded_bytes: 0,
            seek_count: 0,
            hash_node_calls: 0,
            query_duration: Duration::ZERO,
        }
    }
}

/// Result of analyzing a proof for target keys
struct ProofAnalysis {
    /// Keys from the target set that were found in this proof
    found_keys: Vec<Vec<u8>>,
    /// All keys present in the proof (for determining ranges)
    proof_keys: Vec<Vec<u8>>,
}

/// Extract all keys from a proof
fn extract_keys_from_proof(proof: &[Op]) -> Vec<Vec<u8>> {
    let mut keys = Vec::new();
    for op in proof {
        match op {
            Op::Push(node) | Op::PushInverted(node) => {
                if let Some(key) = get_key_from_node(node) {
                    keys.push(key);
                }
            }
            _ => {}
        }
    }
    keys.sort();
    keys
}

/// Get key from a node if it has one
fn get_key_from_node(node: &Node) -> Option<Vec<u8>> {
    match node {
        Node::KV(key, _) => Some(key.clone()),
        Node::KVValueHash(key, ..) => Some(key.clone()),
        Node::KVValueHashFeatureType(key, ..) => Some(key.clone()),
        Node::KVDigest(key, _) => Some(key.clone()),
        Node::KVRefValueHash(key, ..) => Some(key.clone()),
        Node::KVCount(key, ..) => Some(key.clone()),
        Node::KVRefValueHashCount(key, ..) => Some(key.clone()),
        Node::Hash(_) | Node::KVHash(_) | Node::KVHashCount(..) => None,
    }
}

/// Count nodes in a proof
fn count_nodes_in_proof(proof: &[Op]) -> usize {
    proof
        .iter()
        .filter(|op| matches!(op, Op::Push(_) | Op::PushInverted(_)))
        .count()
}

/// Analyze a proof to find target keys.
fn analyze_proof_for_keys(proof: &[Op], target_keys: &BTreeSet<Vec<u8>>) -> ProofAnalysis {
    let proof_keys = extract_keys_from_proof(proof);

    // Find which target keys are in the proof
    let found_keys: Vec<Vec<u8>> = target_keys
        .iter()
        .filter(|k| proof_keys.binary_search(k).is_ok())
        .cloned()
        .collect();

    ProofAnalysis {
        found_keys,
        proof_keys,
    }
}

/// Run the iterative query benchmark
pub fn run_branch_query_benchmark() {
    let grove_version = GroveVersion::latest();
    let mut rng = SmallRng::seed_from_u64(12345);

    println!("=== Branch Query Benchmark ===\n");

    // Configuration
    let num_elements = 1_000_000;
    let batch_size = 10_000;
    let num_batches = num_elements / batch_size;
    let num_existing_keys = 1000;
    let num_nonexistent_keys = 20;
    let max_depth_per_query = 8;

    println!("Configuration:");
    println!("  Elements: {}", num_elements);
    println!("  Batch size: {}", batch_size);
    println!("  Existing keys to find: {}", num_existing_keys);
    println!("  Non-existent keys to find: {}", num_nonexistent_keys);
    println!("  Max depth per query: {}", max_depth_per_query);
    println!();

    // Create CountSumTree
    println!("Creating CountSumTree with {} elements...", num_elements);
    let mut merk = TempMerk::new_with_tree_type(grove_version, TreeType::CountSumTree);

    // Store all keys for later selection
    let mut all_keys: Vec<Vec<u8>> = Vec::with_capacity(num_elements as usize);

    // Track expected aggregates
    let mut expected_count: u64 = 0;
    let mut expected_sum: i64 = 0;

    // Insert elements in batches
    for batch_num in 0..num_batches {
        let mut batch = Vec::with_capacity(batch_size as usize);

        for _ in 0..batch_size {
            // 32-byte random key
            let mut key = [0u8; 32];
            rng.fill(&mut key);

            // Random value between 1 and 20
            let value_num: u8 = rng.gen_range(1..=20);
            let item_value = vec![value_num];

            // Random balance between 1000 and 1000000 for SumItem
            let balance: i64 = rng.gen_range(1000..=1_000_000);

            // Create Element::ItemWithSumItem and serialize it
            let element = Element::new_item_with_sum_item(item_value, balance);
            let serialized_value = element.serialize(grove_version).expect("serialize failed");

            all_keys.push(key.to_vec());

            // Track expected aggregates (each element counts as 1, contributes its balance
            // to sum)
            expected_count += 1;
            expected_sum += balance;

            // Use CountedSummedMerkNode so count is tracked (each item counts as 1)
            batch.push((
                key.to_vec(),
                grovedb_merk::Op::Put(
                    serialized_value,
                    TreeFeatureType::CountedSummedMerkNode(1, balance),
                ),
            ));
        }

        // Sort batch by key (required for apply)
        batch.sort_by(|a, b| a.0.cmp(&b.0));

        merk.apply::<_, Vec<u8>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");

        if (batch_num + 1) % 10 == 0 {
            println!(
                "  Inserted {} elements ({:.1}%)",
                (batch_num + 1) * batch_size,
                ((batch_num + 1) as f64 / num_batches as f64) * 100.0
            );
        }
    }

    // Commit changes
    merk.commit(grove_version);

    println!("Tree created successfully.");
    println!("  Tree height: {:?}", merk.height());
    println!("  Tree type: {:?}", merk.tree_type);

    // Verify the aggregate data matches expected values
    match merk.aggregate_data() {
        Ok(agg) => {
            println!("  Aggregate data: {:?}", agg);
            let actual_count = agg.as_count_u64();
            let actual_sum = agg.as_sum_i64();
            assert_eq!(
                actual_count, expected_count,
                "Count mismatch: expected {}, got {}",
                expected_count, actual_count
            );
            assert_eq!(
                actual_sum, expected_sum,
                "Sum mismatch: expected {}, got {}",
                expected_sum, actual_sum
            );
            println!(
                "  ✓ Aggregate data verified: count={}, sum={}",
                actual_count, actual_sum
            );
        }
        Err(e) => {
            panic!("Failed to get aggregate data: {:?}", e);
        }
    }
    println!();

    // Select random existing keys
    let mut existing_keys: BTreeSet<Vec<u8>> = BTreeSet::new();
    while existing_keys.len() < num_existing_keys {
        let idx = rng.gen_range(0..all_keys.len());
        existing_keys.insert(all_keys[idx].clone());
    }

    // Generate random non-existent keys
    let mut nonexistent_keys: BTreeSet<Vec<u8>> = BTreeSet::new();
    while nonexistent_keys.len() < num_nonexistent_keys {
        let mut key = [0u8; 32];
        rng.fill(&mut key);
        // Make sure it's not in the tree (very unlikely but check anyway)
        if !all_keys.contains(&key.to_vec()) {
            nonexistent_keys.insert(key.to_vec());
        }
    }

    println!(
        "Selected {} existing keys and {} non-existent keys for search\n",
        existing_keys.len(),
        nonexistent_keys.len()
    );

    // Combine all target keys
    let mut all_target_keys: BTreeSet<Vec<u8>> = existing_keys.clone();
    all_target_keys.extend(nonexistent_keys.iter().cloned());

    // Initialize metrics and terminal tracker (uses BST tracing, not value-based
    // boundaries)
    let mut metrics = QueryMetrics::default();
    let mut tracker = KeyTerminalTracker::new();
    let mut iteration = 0;

    // Track all known proof keys
    let mut known_proof_keys: Vec<Vec<u8>> = Vec::new();

    println!("Starting iterative query process...\n");

    // Iterative query process
    loop {
        iteration += 1;
        println!("=== Iteration {} ===", iteration);
        println!("  Remaining keys to find: {}", tracker.remaining_count());

        if iteration == 1 {
            // First iteration: get trunk
            println!(
                "  Performing trunk query with max_depth={}...",
                max_depth_per_query
            );

            let mut query_cost = OperationCost::default();
            let query_start = Instant::now();
            let trunk_query_result = merk
                .trunk_query(max_depth_per_query, None, grove_version)
                .unwrap_add_cost(&mut query_cost);
            metrics.query_duration += query_start.elapsed();
            metrics.storage_loaded_bytes += query_cost.storage_loaded_bytes;
            metrics.seek_count += query_cost.seek_count;
            metrics.hash_node_calls += query_cost.hash_node_calls;

            match trunk_query_result {
                Ok(trunk_result) => {
                    metrics.total_queries += 1;
                    metrics.total_nodes_processed += count_nodes_in_proof(&trunk_result.proof);

                    // Verify all terminal Node::Hash entries are at the expected depth
                    trunk_result
                        .verify_terminal_nodes_at_expected_depth()
                        .expect("Terminal nodes should all be at expected depth");

                    println!("  Trunk query result:");
                    println!("    Tree depth: {}", trunk_result.tree_depth);
                    println!("    Chunk depths: {:?}", trunk_result.chunk_depths);
                    println!("    Proof size: {} ops", trunk_result.proof.len());
                    println!(
                        "    Nodes in proof: {}",
                        count_nodes_in_proof(&trunk_result.proof)
                    );
                    println!(
                        "    Terminal node keys (trunk leaves): {}",
                        trunk_result.terminal_node_keys().len()
                    );

                    // Analyze the proof
                    let analysis = analyze_proof_for_keys(&trunk_result.proof, &all_target_keys);
                    println!("    Keys found in trunk: {}", analysis.found_keys.len());
                    println!("    Keys in proof: {}", analysis.proof_keys.len());

                    known_proof_keys = analysis.proof_keys;

                    // Track found keys
                    let found_keys: BTreeSet<Vec<u8>> =
                        analysis.found_keys.iter().cloned().collect();
                    metrics.keys_found += found_keys.len();

                    // For each target key not found, trace through BST to find its terminal
                    let mut absent_count = 0;
                    for target_key in &all_target_keys {
                        if found_keys.contains(target_key) {
                            continue;
                        }

                        // Trace through the proof's BST structure to find which terminal this key
                        // is under
                        match trunk_result.trace_key_to_terminal(target_key) {
                            Some(terminal) => {
                                tracker.add_key(target_key.clone(), terminal);
                            }
                            None => {
                                // Key traces to no terminal - it doesn't exist
                                metrics.keys_absent += 1;
                                absent_count += 1;
                            }
                        }
                    }

                    let active = tracker.active_terminal_keys();
                    println!(
                        "  {} keys still need branch queries",
                        tracker.remaining_count()
                    );
                    println!("  {} keys proven absent in trunk", absent_count);
                    println!("  {} active terminal keys (refcounted)", active.len());
                }
                Err(e) => {
                    println!("  Trunk query failed: {:?}", e);
                    break;
                }
            }
        } else {
            // Query active terminal keys (those with refcount > 0)
            let terminal_keys_to_query = tracker.active_terminal_keys();

            if terminal_keys_to_query.is_empty() {
                if tracker.remaining_count() > 0 {
                    println!(
                        "  No active terminal keys - {} remaining keys proven absent",
                        tracker.remaining_count()
                    );
                    metrics.keys_absent += tracker.remaining_count();
                }
                break;
            }

            println!(
                "  Querying {} active terminal keys...",
                terminal_keys_to_query.len()
            );

            let remaining_before = tracker.remaining_count();

            for terminal_key in terminal_keys_to_query {
                let mut query_cost = OperationCost::default();
                let query_start = Instant::now();
                let branch_query_result = merk
                    .branch_query(&terminal_key, max_depth_per_query, grove_version)
                    .unwrap_add_cost(&mut query_cost);
                metrics.query_duration += query_start.elapsed();
                metrics.storage_loaded_bytes += query_cost.storage_loaded_bytes;
                metrics.seek_count += query_cost.seek_count;
                metrics.hash_node_calls += query_cost.hash_node_calls;
                metrics.total_queries += 1;

                match branch_query_result {
                    Ok(branch_result) => {
                        metrics.total_nodes_processed += count_nodes_in_proof(&branch_result.proof);
                        let proof_keys = extract_keys_from_proof(&branch_result.proof);

                        // Get keys that were targeting this terminal
                        let keys_for_this_terminal = tracker.keys_for_terminal(&terminal_key);
                        let mut found_in_this_proof = 0;
                        let mut absent_in_this_proof = 0;

                        for check_key in keys_for_this_terminal {
                            if proof_keys.binary_search(&check_key).is_ok() {
                                // Found!
                                tracker.key_found(&check_key);
                                metrics.keys_found += 1;
                                found_in_this_proof += 1;
                            } else {
                                // Not found in proof - trace through to find new terminal or prove
                                // absent
                                match branch_result.trace_key_to_terminal(&check_key) {
                                    Some(new_terminal) => {
                                        // Key is in a deeper subtree
                                        tracker.update_terminal(&check_key, new_terminal);
                                    }
                                    None => {
                                        // Key doesn't exist in tree
                                        tracker.key_found(&check_key);
                                        metrics.keys_absent += 1;
                                        absent_in_this_proof += 1;
                                    }
                                }
                            }
                        }

                        if found_in_this_proof > 0 || absent_in_this_proof > 0 {
                            println!(
                                "    Terminal key {}...: found {} keys, {} absent",
                                hex::encode(&terminal_key[..8.min(terminal_key.len())]),
                                found_in_this_proof,
                                absent_in_this_proof
                            );
                        }

                        // Add proof keys to known set
                        for pk in proof_keys {
                            if known_proof_keys.binary_search(&pk).is_err() {
                                known_proof_keys.push(pk);
                            }
                        }
                        known_proof_keys.sort();
                    }
                    Err(e) => {
                        println!(
                            "    Terminal key {}... query failed: {:?}",
                            hex::encode(&terminal_key[..8.min(terminal_key.len())]),
                            e
                        );
                    }
                }
            }

            // Check progress
            let remaining_after = tracker.remaining_count();
            let made_progress = remaining_after < remaining_before;

            if remaining_after > 0 {
                let active = tracker.active_terminal_keys().len();
                println!(
                    "  {} remaining keys, {} active terminal keys",
                    remaining_after, active
                );

                if !made_progress && active == 0 {
                    // No active terminals and no progress - remaining are absent
                    println!(
                        "  No active terminals - {} remaining keys proven absent",
                        remaining_after
                    );
                    metrics.keys_absent += remaining_after;
                    break;
                }
            }
        }

        println!();

        if tracker.is_empty() {
            break;
        }

        // Safety limit
        if iteration > 100 {
            println!("Reached iteration limit, stopping.");
            break;
        }
    }

    // Print final metrics
    println!("=== Final Metrics ===");
    println!("Total queries: {}", metrics.total_queries);
    println!("Total nodes processed: {}", metrics.total_nodes_processed);
    println!(
        "Unique keys discovered in proofs: {}",
        known_proof_keys.len()
    );
    println!("Keys found: {}", metrics.keys_found);
    println!("Keys proven absent: {}", metrics.keys_absent);
    println!("Remaining unfound keys: {}", tracker.remaining_count());
    println!(
        "Expected: {} found, {} absent",
        num_existing_keys, num_nonexistent_keys
    );

    println!();
    println!("=== Performance Metrics ===");
    println!(
        "Total query time: {:.3}s",
        metrics.query_duration.as_secs_f64()
    );
    println!(
        "Average time per query: {:.3}ms",
        metrics.query_duration.as_secs_f64() * 1000.0 / metrics.total_queries as f64
    );
    println!(
        "Efficiency: {:.1} queries per target key",
        metrics.total_queries as f64 / (num_existing_keys + num_nonexistent_keys) as f64
    );

    println!();
    println!("=== I/O Metrics ===");
    println!(
        "Storage bytes loaded: {} ({:.2} MB)",
        metrics.storage_loaded_bytes,
        metrics.storage_loaded_bytes as f64 / (1024.0 * 1024.0)
    );
    println!("Storage seek operations: {}", metrics.seek_count);
    println!(
        "Average bytes per query: {:.0}",
        metrics.storage_loaded_bytes as f64 / metrics.total_queries as f64
    );
    println!(
        "Average seeks per query: {:.1}",
        metrics.seek_count as f64 / metrics.total_queries as f64
    );

    println!();
    println!("=== Hashing Metrics ===");
    println!("Total hash node calls: {}", metrics.hash_node_calls);
    println!(
        "Average hashes per query: {:.1}",
        metrics.hash_node_calls as f64 / metrics.total_queries as f64
    );
}

fn main() {
    run_branch_query_benchmark();
}
