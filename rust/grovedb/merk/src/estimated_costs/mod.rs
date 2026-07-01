//! Estimated costs for Merk

#[cfg(feature = "minimal")]
use grovedb_costs::OperationCost;
#[cfg(feature = "minimal")]
use integer_encoding::VarInt;

#[cfg(feature = "minimal")]
use crate::merk::NodeType;
#[cfg(feature = "minimal")]
use crate::tree_type::TreeType;
#[cfg(feature = "minimal")]
use crate::{tree::kv::KV, HASH_BLOCK_SIZE_U32, HASH_LENGTH_U32};

#[cfg(feature = "minimal")]
pub mod average_case_costs;

#[cfg(feature = "minimal")]
pub mod worst_case_costs;

#[cfg(feature = "minimal")]
/// The cost of a subtree layer
/// It is 3 because we have:
/// 1 byte for the element type
/// 1 byte for the root key option
/// 1 byte for the flag option
pub const LAYER_COST_SIZE: u32 = 3;

#[cfg(any(feature = "minimal", feature = "verify"))]
/// The cost of a sum value
pub const SUM_VALUE_EXTRA_COST: u32 = 9;

#[cfg(any(feature = "minimal", feature = "verify"))]
/// The cost of a count value
pub const COUNT_VALUE_EXTRA_COST: u32 = 9;

#[cfg(any(feature = "minimal", feature = "verify"))]
/// The cost of a big sum value
pub const BIG_SUM_VALUE_EXTRA_COST: u32 = 16;

#[cfg(feature = "minimal")]
/// The cost of a summed subtree layer
/// This is the layer size + 9 for the encoded value
pub const SUM_LAYER_COST_SIZE: u32 = LAYER_COST_SIZE + SUM_VALUE_EXTRA_COST;

#[cfg(feature = "minimal")]
/// The cost of a summed subtree layer
/// This is the layer size + 9 for the encoded value
pub const SUM_AND_COUNT_LAYER_COST_SIZE: u32 =
    LAYER_COST_SIZE + SUM_VALUE_EXTRA_COST + COUNT_VALUE_EXTRA_COST;

#[cfg(feature = "minimal")]
/// The cost of a summed subtree layer
/// This is the layer size + 16 for the encoded value
pub const BIG_SUM_LAYER_COST_SIZE: u32 = LAYER_COST_SIZE + BIG_SUM_VALUE_EXTRA_COST;

#[cfg(feature = "minimal")]
impl KV {
    fn encoded_kv_node_size(element_size: u32, node_type: NodeType) -> u32 {
        // We always charge 8 bytes for the sum node (even though
        // it could theoretically be 9 bytes
        let sum_node_feature_size = node_type.feature_len();
        // KV holds the state of a node
        // 32 bytes to encode the hash of the node
        // 32 bytes to encode the value hash
        // max_element_size to encode the worst case value size
        HASH_LENGTH_U32 + HASH_LENGTH_U32 + element_size + sum_node_feature_size
    }
}

#[cfg(feature = "minimal")]
/// Add cost case for insertion into merk
pub fn add_cost_case_merk_insert(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    in_tree_type: TreeType,
) {
    cost.seek_count += 1;
    cost.storage_cost.added_bytes += KV::node_byte_cost_size_for_key_and_raw_value_lengths(
        key_len,
        value_len,
        in_tree_type.inner_node_type(),
    );
    // .. and hash computation for the inserted element itself
    // first lets add the value hash
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the kv_digest_to_kv_hash hash call
    let hashed_size = key_len.encode_var_vec().len() as u32 + key_len + HASH_LENGTH_U32;
    cost.hash_node_calls += 1 + ((hashed_size - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the two block hashes for the node hash call
    cost.hash_node_calls += 2;
}

#[cfg(feature = "minimal")]
/// Add cost case for insertion into merk
pub fn add_cost_case_merk_insert_layered(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    in_tree_type: TreeType,
) {
    cost.seek_count += 1;
    cost.storage_cost.added_bytes += KV::layered_node_byte_cost_size_for_key_and_value_lengths(
        key_len,
        value_len,
        in_tree_type.inner_node_type(),
    );
    // .. and hash computation for the inserted element itself
    // first lets add the value hash
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the combine hash
    cost.hash_node_calls += 1;
    // then let's add the kv_digest_to_kv_hash hash call
    let hashed_size = key_len.encode_var_vec().len() as u32 + key_len + HASH_LENGTH_U32;
    cost.hash_node_calls += 1 + ((hashed_size - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the two block hashes for the node hash call
    cost.hash_node_calls += 2;
}

#[cfg(feature = "minimal")]
/// Add cost case for insertion into merk
pub fn add_cost_case_merk_replace(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    in_tree_type: TreeType,
) {
    cost.seek_count += 1;
    cost.storage_cost.added_bytes +=
        KV::node_value_byte_cost_size(key_len, value_len, in_tree_type.inner_node_type());
    cost.storage_cost.replaced_bytes += KV::node_key_byte_cost_size(key_len);
    // .. and hash computation for the inserted element itself
    // first lets add the value hash
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the kv_digest_to_kv_hash hash call
    let hashed_size = key_len.encode_var_vec().len() as u32 + key_len + HASH_LENGTH_U32;
    cost.hash_node_calls += 1 + ((hashed_size - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the two block hashes for the node hash call
    cost.hash_node_calls += 2;
}

#[cfg(feature = "minimal")]
/// Add cost case for replacement in merk when the value size is known to not
/// change
pub fn add_cost_case_merk_replace_same_size(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    in_tree_type: TreeType,
) {
    cost.seek_count += 1;
    cost.storage_cost.replaced_bytes += KV::node_byte_cost_size_for_key_and_raw_value_lengths(
        key_len,
        value_len,
        in_tree_type.inner_node_type(),
    );
    // .. and hash computation for the inserted element itself
    // first lets add the value hash
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the kv_digest_to_kv_hash hash call
    let hashed_size = key_len.encode_var_vec().len() as u32 + key_len + HASH_LENGTH_U32;
    cost.hash_node_calls += 1 + ((hashed_size - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the two block hashes for the node hash call
    cost.hash_node_calls += 2;
}

#[cfg(feature = "minimal")]
/// Add cost case for insertion into merk
pub fn add_cost_case_merk_replace_layered(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    in_tree_type: TreeType,
) {
    cost.seek_count += 1;
    cost.storage_cost.replaced_bytes += KV::layered_node_byte_cost_size_for_key_and_value_lengths(
        key_len,
        value_len,
        in_tree_type.inner_node_type(),
    );
    // .. and hash computation for the inserted element itself
    // first lets add the value hash
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the combine hash
    cost.hash_node_calls += 1;
    // then let's add the kv_digest_to_kv_hash hash call
    let hashed_size = key_len.encode_var_vec().len() as u32 + key_len + HASH_LENGTH_U32;
    cost.hash_node_calls += 1 + ((hashed_size - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the two block hashes for the node hash call
    cost.hash_node_calls += 2;
}

#[cfg(feature = "minimal")]
/// Add cost case for replacement in merk when the value size is known to not
/// change
pub fn add_cost_case_merk_patch(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    change_in_bytes: i32,
    in_tree_type: TreeType,
) {
    cost.seek_count += 1;
    if change_in_bytes >= 0 {
        // it's possible that the required space has also changed which would cause a +1
        // to happen
        let old_byte_size = KV::node_byte_cost_size_for_key_and_raw_value_lengths(
            key_len,
            value_len - change_in_bytes as u32,
            in_tree_type.inner_node_type(),
        );
        let new_byte_size = KV::node_byte_cost_size_for_key_and_raw_value_lengths(
            key_len,
            value_len,
            in_tree_type.inner_node_type(),
        );
        cost.storage_cost.replaced_bytes += old_byte_size;

        cost.storage_cost.added_bytes += new_byte_size - old_byte_size;
    } else {
        cost.storage_cost.replaced_bytes += KV::node_byte_cost_size_for_key_and_raw_value_lengths(
            key_len,
            value_len,
            in_tree_type.inner_node_type(),
        );
    }

    // .. and hash computation for the inserted element itself
    // first lets add the value hash
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the kv_digest_to_kv_hash hash call
    let hashed_size = key_len.encode_var_vec().len() as u32 + key_len + HASH_LENGTH_U32;
    cost.hash_node_calls += 1 + ((hashed_size - 1) / HASH_BLOCK_SIZE_U32);
    // then let's add the two block hashes for the node hash call
    cost.hash_node_calls += 2;
}
