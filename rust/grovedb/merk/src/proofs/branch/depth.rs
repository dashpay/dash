//! Depth calculation utilities for branch queries.
//!
//! This module provides functions for calculating tree depth from element count
//! and for calculating optimal chunk depth splitting.

/// Calculate the maximum possible height of an AVL tree from its element count.
///
/// AVL trees have a worst-case height based on Fibonacci numbers. The minimum
/// number of nodes for an AVL tree of height h is `N(h) = F(h+2) - 1`, where F
/// is the Fibonacci sequence.
///
/// This function returns the maximum height an AVL tree with `count` nodes
/// could have, which is the largest h where `N(h) <= count`.
///
/// Reference values for N(h):
/// - N(1)=1, N(2)=2, N(3)=4, N(4)=7, N(5)=12, N(6)=20, N(7)=33
/// - N(8)=54, N(9)=88, N(10)=143, N(11)=232, N(12)=376
///
/// # Arguments
/// * `count` - The number of elements in the tree
///
/// # Returns
/// The maximum possible height of the tree as a u8
///
/// # Examples
/// ```
/// use grovedb_merk::proofs::branch::depth::calculate_max_tree_depth_from_count;
///
/// assert_eq!(calculate_max_tree_depth_from_count(0), 0);
/// assert_eq!(calculate_max_tree_depth_from_count(1), 1); // N(1)=1
/// assert_eq!(calculate_max_tree_depth_from_count(2), 2); // N(2)=2
/// assert_eq!(calculate_max_tree_depth_from_count(4), 3); // N(3)=4
/// assert_eq!(calculate_max_tree_depth_from_count(7), 4); // N(4)=7
/// assert_eq!(calculate_max_tree_depth_from_count(12), 5); // N(5)=12
/// assert_eq!(calculate_max_tree_depth_from_count(88), 9); // N(9)=88
/// assert_eq!(calculate_max_tree_depth_from_count(100), 9); // 88 <= 100 < 143
/// ```
pub fn calculate_max_tree_depth_from_count(count: u64) -> u8 {
    if count == 0 {
        return 0;
    }

    // Fibonacci: F(1)=1, F(2)=1, F(3)=2, F(4)=3, F(5)=5, ...
    // Minimum nodes for AVL height h: N(h) = F(h+2) - 1
    // We find the largest h where N(h) <= count.

    let mut f_prev: u64 = 1; // F(2)
    let mut f_curr: u64 = 2; // F(3)
    let mut height: u8 = 1;

    loop {
        // Calculate N(height+1) = F(height+3) - 1
        let f_next = f_prev.saturating_add(f_curr);
        let next_min_nodes = f_next.saturating_sub(1);

        if next_min_nodes > count {
            // height+1 would require more nodes than we have
            return height;
        }

        // Move to next height
        height += 1;
        f_prev = f_curr;
        f_curr = f_next;

        // F(93) overflows u64, so cap at height 92
        if height >= 92 {
            return height;
        }
    }
}

/// Calculate chunk depths with minimum depth constraint for provable count
/// trees.
///
/// Distributes tree depth evenly across chunks, with front chunks getting
/// priority for any extra. When splitting is needed, the first chunk is
/// at least `min_depth` for privacy.
///
/// If tree_depth <= max_depth, returns `[tree_depth]` (single chunk, no
/// splitting).
///
/// # Arguments
/// * `tree_depth` - Total depth of the tree (from count)
/// * `max_depth` - Maximum depth per chunk
/// * `min_depth` - Minimum depth for first chunk when splitting (for privacy)
///
/// # Returns
/// A vector of chunk depths that sum to tree_depth
///
/// # Examples
/// ```
/// use grovedb_merk::proofs::branch::depth::calculate_chunk_depths_with_minimum;
///
/// // depth=10, max=8, min=6: first chunk bumped to min
/// assert_eq!(calculate_chunk_depths_with_minimum(10, 8, 6), vec![6, 4]);
///
/// // depth=11, max=8, min=6: even split, front gets extra
/// assert_eq!(calculate_chunk_depths_with_minimum(11, 8, 6), vec![6, 5]);
///
/// // depth=13, max=8, min=6: front chunk gets the extra
/// assert_eq!(calculate_chunk_depths_with_minimum(13, 8, 6), vec![7, 6]);
///
/// // depth=14, max=8, min=6: even split
/// assert_eq!(calculate_chunk_depths_with_minimum(14, 8, 6), vec![7, 7]);
///
/// // depth=4, max=10, min=6: fits in single chunk, return as-is
/// assert_eq!(calculate_chunk_depths_with_minimum(4, 10, 6), vec![4]);
/// ```
pub fn calculate_chunk_depths_with_minimum(
    tree_depth: u8,
    max_depth: u8,
    min_depth: u8,
) -> Vec<u8> {
    if max_depth == 0 {
        panic!("max_depth must be > 0");
    }
    if min_depth == 0 {
        panic!("min_depth must be > 0");
    }
    if min_depth > max_depth {
        panic!("min_depth must be <= max_depth");
    }

    // Single chunk if it fits within max (no splitting needed, min_depth doesn't
    // apply)
    if tree_depth <= max_depth {
        return vec![tree_depth];
    }

    // Calculate minimum number of chunks needed
    let num_chunks = (tree_depth as u32).div_ceil(max_depth as u32);

    let mut chunks = Vec::with_capacity(num_chunks as usize);
    let mut remaining = tree_depth;

    for i in 0..num_chunks {
        let chunks_left = num_chunks - i;
        // Base even share for remaining chunks
        let base = remaining / chunks_left as u8;
        let has_extra = (remaining % chunks_left as u8) > 0;

        // Front chunks get extra, first chunk at least min_depth
        let chunk = if has_extra { base + 1 } else { base };
        let chunk = if i == 0 {
            chunk.max(min_depth).min(max_depth)
        } else {
            chunk.min(max_depth)
        };

        chunks.push(chunk);
        remaining -= chunk;
    }

    chunks
}

/// Calculate optimal chunk depths for even splitting of a tree.
///
/// Instead of naive splitting like `[8, 8, 4]` for tree_depth=20 with
/// max_depth=8, this distributes depths evenly like `[7, 7, 6]`.
///
/// # Arguments
/// * `tree_depth` - Total depth of the tree
/// * `max_depth` - Maximum depth per chunk
///
/// # Returns
/// A vector of chunk depths that sum to `tree_depth`, where each depth is <=
/// `max_depth`
///
/// # Examples
/// ```
/// use grovedb_merk::proofs::branch::depth::calculate_chunk_depths;
///
/// assert_eq!(calculate_chunk_depths(20, 8), vec![7, 7, 6]);
/// assert_eq!(calculate_chunk_depths(15, 5), vec![5, 5, 5]);
/// assert_eq!(calculate_chunk_depths(10, 4), vec![4, 3, 3]);
/// assert_eq!(calculate_chunk_depths(5, 10), vec![5]);
/// ```
pub fn calculate_chunk_depths(tree_depth: u8, max_depth: u8) -> Vec<u8> {
    if max_depth == 0 {
        panic!("max_depth must be > 0");
    }

    if tree_depth == 0 {
        return vec![0];
    }

    if tree_depth <= max_depth {
        return vec![tree_depth];
    }

    // Calculate number of chunks needed: ceil(tree_depth / max_depth)
    let num_chunks = (tree_depth as u32).div_ceil(max_depth as u32);

    // Calculate base depth per chunk and remainder
    let base_depth = (tree_depth as u32) / num_chunks;
    let remainder = (tree_depth as u32) % num_chunks;

    // Distribute remainder across chunks for even distribution
    // Higher depth chunks come first (they represent higher tree levels)
    (0..num_chunks)
        .map(|i| {
            if i < remainder {
                (base_depth + 1) as u8
            } else {
                base_depth as u8
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_calculate_tree_depth_from_count_edge_cases() {
        assert_eq!(calculate_max_tree_depth_from_count(0), 0);
        assert_eq!(calculate_max_tree_depth_from_count(1), 1);
    }

    /// Verifies that calculated max depth is always >= actual merk tree height
    /// when inserting sequential keys.
    #[test]
    fn test_calculate_tree_depth_vs_actual_merk_height_sequential_keys() {
        use grovedb_version::version::GroveVersion;

        use crate::{test_utils::TempMerk, tree::Op, TreeFeatureType::BasicMerkNode};

        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        for i in 0u32..130 {
            let key = i.to_be_bytes().to_vec();
            let value = vec![i as u8];

            merk.apply::<_, Vec<_>>(
                &[(key.clone(), Op::Put(value, BasicMerkNode))],
                &[],
                None,
                grove_version,
            )
            .unwrap()
            .expect("apply should succeed");

            merk.commit(grove_version);

            let count = (i + 1) as u64;
            let calculated = calculate_max_tree_depth_from_count(count);
            let actual_height = merk.height().unwrap_or(0);

            assert!(
                calculated >= actual_height,
                "calculated max depth {} should be >= actual height {} for count {}",
                calculated,
                actual_height,
                count
            );
        }
    }

    /// Verifies that calculated max depth is always >= actual merk tree height
    /// when inserting random hash keys in sorted order.
    #[test]
    fn test_calculate_tree_depth_vs_actual_merk_height_random_hash_keys_sorted() {
        use grovedb_version::version::GroveVersion;

        use crate::{test_utils::TempMerk, tree::Op, TreeFeatureType::BasicMerkNode};

        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        // Pre-generate and sort keys
        let mut keys_with_index: Vec<(Vec<u8>, u32)> = (0u32..130)
            .map(|i| {
                let hash = blake3::hash(&i.to_be_bytes());
                (hash.as_bytes().to_vec(), i)
            })
            .collect();
        keys_with_index.sort_by(|a, b| a.0.cmp(&b.0));

        for (idx, (key, original_i)) in keys_with_index.into_iter().enumerate() {
            let value = vec![original_i as u8];

            merk.apply::<_, Vec<_>>(
                &[(key, Op::Put(value, BasicMerkNode))],
                &[],
                None,
                grove_version,
            )
            .unwrap()
            .expect("apply should succeed");

            merk.commit(grove_version);

            let count = (idx + 1) as u64;
            let calculated = calculate_max_tree_depth_from_count(count);
            let actual_height = merk.height().unwrap_or(0);

            assert!(
                calculated >= actual_height,
                "calculated max depth {} should be >= actual height {} for count {}",
                calculated,
                actual_height,
                count
            );
        }
    }

    /// Verifies that calculated max depth is always >= actual merk tree height
    /// when inserting random hash keys in unsorted order (simulating real-world
    /// usage).
    #[test]
    fn test_calculate_tree_depth_vs_actual_merk_height_random_hash_keys_unsorted() {
        use grovedb_version::version::GroveVersion;

        use crate::{test_utils::TempMerk, tree::Op, TreeFeatureType::BasicMerkNode};

        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        // Insert keys one at a time in hash order (simulates arbitrary insertion order)
        for i in 0u32..130 {
            let hash = blake3::hash(&i.to_be_bytes());
            let key = hash.as_bytes().to_vec();
            let value = vec![i as u8];

            merk.apply::<_, Vec<_>>(
                &[(key, Op::Put(value, BasicMerkNode))],
                &[],
                None,
                grove_version,
            )
            .unwrap()
            .expect("apply should succeed");

            merk.commit(grove_version);

            let count = (i + 1) as u64;
            let calculated = calculate_max_tree_depth_from_count(count);
            let actual_height = merk.height().unwrap_or(0);

            assert!(
                calculated >= actual_height,
                "calculated max depth {} should be >= actual height {} for count {}",
                calculated,
                actual_height,
                count
            );
        }
    }

    #[test]
    fn test_calculate_tree_depth_from_count_fibonacci_boundaries() {
        // AVL max height follows Fibonacci: N(h) = F(h+2) - 1
        // N(1)=1, N(2)=2, N(3)=4, N(4)=7, N(5)=12, N(6)=20, N(7)=33, N(8)=54, N(9)=88
        assert_eq!(calculate_max_tree_depth_from_count(1), 1); // N(1)=1
        assert_eq!(calculate_max_tree_depth_from_count(2), 2); // N(2)=2
        assert_eq!(calculate_max_tree_depth_from_count(3), 2); // N(2)=2 <= 3 < N(3)=4
        assert_eq!(calculate_max_tree_depth_from_count(4), 3); // N(3)=4
        assert_eq!(calculate_max_tree_depth_from_count(7), 4); // N(4)=7
        assert_eq!(calculate_max_tree_depth_from_count(12), 5); // N(5)=12
        assert_eq!(calculate_max_tree_depth_from_count(20), 6); // N(6)=20
        assert_eq!(calculate_max_tree_depth_from_count(33), 7); // N(7)=33
        assert_eq!(calculate_max_tree_depth_from_count(54), 8); // N(8)=54
        assert_eq!(calculate_max_tree_depth_from_count(88), 9); // N(9)=88
    }

    #[test]
    fn test_calculate_tree_depth_from_count_between_boundaries() {
        // Values between Fibonacci boundaries use the lower height
        assert_eq!(calculate_max_tree_depth_from_count(5), 3); // N(3)=4 <= 5 < N(4)=7
        assert_eq!(calculate_max_tree_depth_from_count(6), 3); // N(3)=4 <= 6 < N(4)=7
        assert_eq!(calculate_max_tree_depth_from_count(10), 4); // N(4)=7 <= 10 < N(5)=12
        assert_eq!(calculate_max_tree_depth_from_count(15), 5); // N(5)=12 <= 15 < N(6)=20
        assert_eq!(calculate_max_tree_depth_from_count(50), 7); // N(7)=33 <= 50 < N(8)=54
        assert_eq!(calculate_max_tree_depth_from_count(100), 9); // N(9)=88 <=
                                                                 // 100 < N(10)=143
    }

    #[test]
    fn test_calculate_tree_depth_from_count_large_values() {
        // N(14)=986, N(15)=1596
        assert_eq!(calculate_max_tree_depth_from_count(1000), 14);
        // N(28)=832039, N(29)=1346268
        assert_eq!(calculate_max_tree_depth_from_count(1_000_000), 28);
        // N(42)=701408732, N(43)=1134903169
        assert_eq!(calculate_max_tree_depth_from_count(1_000_000_000), 42);
    }

    #[test]
    fn test_calculate_chunk_depths_no_splitting_needed() {
        assert_eq!(calculate_chunk_depths(5, 10), vec![5]);
        assert_eq!(calculate_chunk_depths(8, 8), vec![8]);
        assert_eq!(calculate_chunk_depths(3, 5), vec![3]);
    }

    #[test]
    fn test_calculate_chunk_depths_even_split() {
        assert_eq!(calculate_chunk_depths(15, 5), vec![5, 5, 5]);
        assert_eq!(calculate_chunk_depths(20, 10), vec![10, 10]);
        assert_eq!(calculate_chunk_depths(12, 4), vec![4, 4, 4]);
    }

    #[test]
    fn test_calculate_chunk_depths_uneven_split() {
        // 20 / 8 = 2.5, so 3 chunks needed
        // 20 / 3 = 6 remainder 2, so [7, 7, 6]
        assert_eq!(calculate_chunk_depths(20, 8), vec![7, 7, 6]);

        // 10 / 4 = 2.5, so 3 chunks needed
        // 10 / 3 = 3 remainder 1, so [4, 3, 3]
        assert_eq!(calculate_chunk_depths(10, 4), vec![4, 3, 3]);

        // 17 / 5 = 3.4, so 4 chunks needed
        // 17 / 4 = 4 remainder 1, so [5, 4, 4, 4]
        assert_eq!(calculate_chunk_depths(17, 5), vec![5, 4, 4, 4]);
    }

    #[test]
    fn test_calculate_chunk_depths_edge_cases() {
        assert_eq!(calculate_chunk_depths(0, 8), vec![0]);
        assert_eq!(calculate_chunk_depths(1, 1), vec![1]);
    }

    #[test]
    #[should_panic(expected = "max_depth must be > 0")]
    fn test_calculate_chunk_depths_zero_max_depth_panics() {
        calculate_chunk_depths(5, 0);
    }

    #[test]
    fn test_chunk_depths_sum_to_tree_depth() {
        for tree_depth in 1..50u8 {
            for max_depth in 1..20u8 {
                let chunks = calculate_chunk_depths(tree_depth, max_depth);
                let sum: u8 = chunks.iter().sum();
                assert_eq!(
                    sum, tree_depth,
                    "Chunks {:?} should sum to {} for max_depth {}",
                    chunks, tree_depth, max_depth
                );
            }
        }
    }

    #[test]
    fn test_chunk_depths_all_within_max() {
        for tree_depth in 1..50u8 {
            for max_depth in 1..20u8 {
                let chunks = calculate_chunk_depths(tree_depth, max_depth);
                for chunk in &chunks {
                    assert!(
                        *chunk <= max_depth,
                        "Chunk depth {} exceeds max_depth {} for tree_depth {}",
                        chunk,
                        max_depth,
                        tree_depth
                    );
                }
            }
        }
    }

    // Tests for calculate_chunk_depths_with_minimum

    #[test]
    fn test_chunk_depths_with_minimum_basic_cases() {
        // tree_depth=10, max=8, min=6: first chunk bumped to min, remainder to second
        assert_eq!(calculate_chunk_depths_with_minimum(10, 8, 6), vec![6, 4]);

        // tree_depth=11, max=8, min=6: even split with front getting extra
        assert_eq!(calculate_chunk_depths_with_minimum(11, 8, 6), vec![6, 5]);

        // tree_depth=13, max=8, min=6: front chunk gets the extra
        assert_eq!(calculate_chunk_depths_with_minimum(13, 8, 6), vec![7, 6]);

        // tree_depth=14, max=8, min=6: even split
        assert_eq!(calculate_chunk_depths_with_minimum(14, 8, 6), vec![7, 7]);
    }

    #[test]
    fn test_chunk_depths_with_minimum_single_chunk() {
        // If tree fits in max_depth, single chunk
        assert_eq!(calculate_chunk_depths_with_minimum(5, 8, 3), vec![5]);
        assert_eq!(calculate_chunk_depths_with_minimum(8, 8, 6), vec![8]);
    }

    #[test]
    fn test_chunk_depths_with_minimum_small_tree() {
        // If tree_depth fits in max_depth, return as single chunk (no splitting)
        // min_depth only applies when splitting is needed
        assert_eq!(calculate_chunk_depths_with_minimum(4, 10, 6), vec![4]);
        assert_eq!(calculate_chunk_depths_with_minimum(3, 8, 5), vec![3]);
        assert_eq!(calculate_chunk_depths_with_minimum(6, 10, 6), vec![6]);
    }

    #[test]
    fn test_chunk_depths_with_minimum_front_always_biggest() {
        // Front chunk should always be >= later chunks
        for tree_depth in 10..30u8 {
            let chunks = calculate_chunk_depths_with_minimum(tree_depth, 8, 6);
            for i in 1..chunks.len() {
                assert!(
                    chunks[0] >= chunks[i],
                    "Front chunk {} should be >= chunk[{}]={} for tree_depth={}",
                    chunks[0],
                    i,
                    chunks[i],
                    tree_depth
                );
            }
        }
    }

    #[test]
    fn test_chunk_depths_with_minimum_first_chunk_at_least_min_when_splitting() {
        // First chunk should be >= min_depth when splitting is needed (tree_depth >
        // max_depth)
        for tree_depth in 9..30u8 {
            // tree_depth > 8 means splitting is needed
            let chunks = calculate_chunk_depths_with_minimum(tree_depth, 8, 6);
            if chunks.len() > 1 {
                assert!(
                    chunks[0] >= 6,
                    "First chunk {} should be >= min_depth 6 for tree_depth={} (when splitting)",
                    chunks[0],
                    tree_depth
                );
            }
        }
    }

    #[test]
    fn test_chunk_depths_with_minimum_sum_equals_tree_depth() {
        // Chunks should sum to tree_depth
        for tree_depth in 1..30u8 {
            let min_depth = 6u8;
            let max_depth = 8u8;
            let chunks = calculate_chunk_depths_with_minimum(tree_depth, max_depth, min_depth);
            let sum: u8 = chunks.iter().sum();
            assert_eq!(
                sum, tree_depth,
                "Chunks {:?} should sum to {} for tree_depth={}",
                chunks, tree_depth, tree_depth
            );
        }
    }

    #[test]
    fn test_chunk_depths_with_minimum_all_within_max() {
        // All chunks should be <= max_depth
        for tree_depth in 10..50u8 {
            let chunks = calculate_chunk_depths_with_minimum(tree_depth, 8, 6);
            for chunk in &chunks {
                assert!(
                    *chunk <= 8,
                    "Chunk {} exceeds max_depth 8 for tree_depth={}",
                    chunk,
                    tree_depth
                );
            }
        }
    }

    #[test]
    #[should_panic(expected = "min_depth must be <= max_depth")]
    fn test_chunk_depths_with_minimum_min_greater_than_max_panics() {
        calculate_chunk_depths_with_minimum(10, 5, 8);
    }
}
