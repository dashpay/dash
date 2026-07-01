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

//! Worst case costs for Merk

use std::cmp::Ordering;

#[cfg(feature = "minimal")]
use grovedb_costs::{CostResult, CostsExt, OperationCost};

#[cfg(feature = "minimal")]
use crate::merk::NodeType;
#[cfg(feature = "minimal")]
use crate::{
    error::Error,
    merk::defaults::MAX_PREFIXED_KEY_SIZE,
    tree::{kv::KV, Link, TreeNode},
    HASH_BLOCK_SIZE, HASH_BLOCK_SIZE_U32, HASH_LENGTH,
};

#[cfg(feature = "minimal")]
#[derive(Clone, PartialEq, Eq, Debug)]
/// Worst case layer info
pub enum WorstCaseLayerInformation {
    /// Max elements number
    MaxElementsNumber(u32),
    /// Number of levels
    NumberOfLevels(u32),
}

#[cfg(feature = "minimal")]
impl TreeNode {
    /// Return worst case size of encoded tree
    pub fn worst_case_encoded_tree_size(
        not_prefixed_key_len: u32,
        max_element_size: u32,
        node_type: NodeType,
    ) -> u32 {
        // two option values for the left and right link
        // the actual left and right link encoding size
        // the encoded kv node size
        2 + (2 * Link::encoded_link_size(not_prefixed_key_len, node_type))
            + KV::encoded_kv_node_size(max_element_size, node_type)
    }
}

#[cfg(feature = "minimal")]
/// Add worst case for getting a merk node
pub fn add_worst_case_get_merk_node(
    cost: &mut OperationCost,
    not_prefixed_key_len: u32,
    max_element_size: u32,
    node_type: NodeType,
) -> Result<(), Error> {
    // Worst case scenario, the element is not already in memory.
    // One direct seek has to be performed to read the node from storage.
    cost.seek_count += 1;

    // To write a node to disk, the left link, right link and kv nodes are encoded.
    // worst case, the node has both the left and right link present.
    cost.storage_loaded_bytes +=
        TreeNode::worst_case_encoded_tree_size(not_prefixed_key_len, max_element_size, node_type)
            as u64;
    Ok(())
}

#[cfg(feature = "minimal")]
/// Add worst case for getting a merk tree
pub fn add_worst_case_merk_has_value(
    cost: &mut OperationCost,
    not_prefixed_key_len: u32,
    max_element_size: u32,
) {
    cost.seek_count += 1;
    cost.storage_loaded_bytes += not_prefixed_key_len as u64 + max_element_size as u64;
}

#[cfg(feature = "minimal")]
/// Add worst case for insertion into merk
pub fn add_worst_case_merk_insert(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    node_type: NodeType,
) {
    cost.storage_cost.added_bytes +=
        KV::node_byte_cost_size_for_key_and_raw_value_lengths(key_len, value_len, node_type);
    // .. and hash computation for the inserted element itself
    // todo: verify this
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
}

#[cfg(feature = "minimal")]
/// Add worst case for insertion into merk
pub fn add_worst_case_merk_replace_layered(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    node_type: NodeType,
) {
    // todo: verify this
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
    cost.storage_cost.replaced_bytes =
        KV::layered_value_byte_cost_size_for_key_and_value_lengths(key_len, value_len, node_type);
    // 37 + 35 + key_len
}

#[cfg(feature = "minimal")]
/// Add average case for deletion from merk
pub fn add_worst_case_merk_delete_layered(cost: &mut OperationCost, _key_len: u32, value_len: u32) {
    // todo: verify this
    cost.seek_count += 1;
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
}

#[cfg(feature = "minimal")]
/// Add average case for deletion from merk
pub fn add_worst_case_merk_delete(cost: &mut OperationCost, _key_len: u32, value_len: u32) {
    // todo: verify this
    cost.seek_count += 1;
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
}

#[cfg(feature = "minimal")]
const fn node_hash_update_count() -> u32 {
    // It's a hash of node hash, left and right
    let bytes = HASH_LENGTH * 3;
    // todo: verify this

    1 + ((bytes - 1) / HASH_BLOCK_SIZE) as u32
}

#[cfg(feature = "minimal")]
/// Add worst case for getting a merk tree root hash
pub fn add_worst_case_merk_root_hash(cost: &mut OperationCost) {
    cost.hash_node_calls += node_hash_update_count();
}

#[cfg(feature = "minimal")]
/// Merk biggest value size
pub const MERK_BIGGEST_VALUE_SIZE: u32 = u16::MAX as u32;
#[cfg(feature = "minimal")]
/// Merk biggest key size
pub const MERK_BIGGEST_KEY_SIZE: u32 = 256;

#[cfg(feature = "minimal")]
/// Worst case cost of a merk propagation
pub fn worst_case_merk_propagate(input: &WorstCaseLayerInformation) -> CostResult<(), Error> {
    let mut cost = OperationCost::default();
    add_worst_case_merk_propagate(&mut cost, input).wrap_with_cost(cost)
}

#[cfg(feature = "minimal")]
/// Add worst case cost of a merk propagation
pub fn add_worst_case_merk_propagate(
    cost: &mut OperationCost,
    input: &WorstCaseLayerInformation,
) -> Result<(), Error> {
    let mut nodes_updated = 0;
    // Propagation requires to recompute and write hashes up to the root
    let levels = match input {
        WorstCaseLayerInformation::MaxElementsNumber(n) => {
            if *n == u32::MAX {
                32
            } else {
                ((*n + 1) as f32).log2().ceil() as u32
            }
        }
        WorstCaseLayerInformation::NumberOfLevels(n) => *n,
    };
    nodes_updated += levels;

    match levels.cmp(&2) {
        Ordering::Equal => {
            // we can get about 1 rotation, if there are more than 2 levels
            nodes_updated += 1;
        }
        Ordering::Greater => {
            // In AVL tree two rotation may happen at most on insertion, some of them may
            // update one more node except one we already have on our path to the
            // root, thus two more updates.
            nodes_updated += 2;
        }
        _ => {}
    }

    // todo: verify these numbers
    cost.storage_cost.replaced_bytes += nodes_updated * MERK_BIGGEST_VALUE_SIZE;
    cost.storage_loaded_bytes +=
        nodes_updated as u64 * (MERK_BIGGEST_VALUE_SIZE + MERK_BIGGEST_KEY_SIZE) as u64;
    cost.seek_count += nodes_updated;
    cost.hash_node_calls += nodes_updated * 2;
    Ok(())
}

#[cfg(feature = "minimal")]
/// Add worst case cost for is_empty_tree_except
pub fn add_worst_case_cost_for_is_empty_tree_except(
    cost: &mut OperationCost,
    except_keys_count: u16,
) {
    cost.seek_count += except_keys_count as u32 + 1;
    cost.storage_loaded_bytes += MAX_PREFIXED_KEY_SIZE * (except_keys_count as u64 + 1);
}

/// Add average case cost for is_empty_tree_except
#[cfg(feature = "minimal")]
pub fn add_average_case_cost_for_is_empty_tree_except(
    cost: &mut OperationCost,
    except_keys_count: u16,
    estimated_prefixed_key_size: u32,
) {
    cost.seek_count += except_keys_count as u32 + 1;
    cost.storage_loaded_bytes +=
        estimated_prefixed_key_size as u64 * (except_keys_count as u64 + 1);
}
