//! Average case costs for Merk

#[cfg(feature = "minimal")]
use grovedb_costs::{CostResult, CostsExt, OperationCost};
#[cfg(feature = "minimal")]
use grovedb_version::{check_grovedb_v0_or_v1, error::GroveVersionError, version::GroveVersion};
#[cfg(feature = "minimal")]
use integer_encoding::VarInt;

#[cfg(feature = "minimal")]
use crate::{
    error::Error,
    estimated_costs::LAYER_COST_SIZE,
    tree::{kv::KV, Link, TreeNode},
    HASH_BLOCK_SIZE, HASH_BLOCK_SIZE_U32, HASH_LENGTH, HASH_LENGTH_U32,
};
use crate::{merk::NodeType, tree_type::TreeType};

#[cfg(feature = "minimal")]
/// Average key size
pub type AverageKeySize = u8;
#[cfg(feature = "minimal")]
/// Average value size
pub type AverageValueSize = u32;
#[cfg(feature = "minimal")]
/// Average flags size
pub type AverageFlagsSize = u32;
#[cfg(feature = "minimal")]
/// Weight
pub type Weight = u8;

#[cfg(feature = "minimal")]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
/// Estimated number of sum trees
#[derive(Default)]
pub enum EstimatedSumTrees {
    /// No sum trees
    #[default]
    NoSumTrees,
    /// Some sum trees
    SomeSumTrees {
        /// Sum trees weight
        sum_trees_weight: Weight,
        /// Big Sum trees weight
        big_sum_trees_weight: Weight,
        /// Count trees weight
        count_trees_weight: Weight,
        /// Count Sum trees weight
        count_sum_trees_weight: Weight,
        /// Non sum trees weight
        non_sum_trees_weight: Weight,
    },
    /// All sum trees
    AllSumTrees,
    /// All big sum trees
    AllBigSumTrees,
    /// All count trees
    AllCountTrees,
    /// All count sum trees
    AllCountSumTrees,
}

#[cfg(feature = "minimal")]
impl EstimatedSumTrees {
    fn estimated_size(&self, grove_version: &GroveVersion) -> Result<u32, Error> {
        let version = check_grovedb_v0_or_v1!(
            "EstimatedSumTrees::estimated_size",
            grove_version
                .merk_versions
                .average_case_costs
                .sum_tree_estimated_size
        );
        match self {
            EstimatedSumTrees::NoSumTrees => Ok(0),
            EstimatedSumTrees::SomeSumTrees {
                sum_trees_weight,
                big_sum_trees_weight,
                count_trees_weight,
                count_sum_trees_weight,
                non_sum_trees_weight,
            } => {
                // Example calculation including new weights
                let total_weight = *sum_trees_weight as u32
                    + *big_sum_trees_weight as u32
                    + *count_trees_weight as u32
                    + *count_sum_trees_weight as u32
                    + *non_sum_trees_weight as u32;
                if total_weight == 0 {
                    return Err(Error::DivideByZero("weights add up to 0"));
                };
                if version == 0 {
                    Ok((*non_sum_trees_weight as u32 * 9)
                        / (*sum_trees_weight as u32 + *non_sum_trees_weight as u32))
                } else if version == 1 {
                    let estimated_size = (*sum_trees_weight as u32
                        * TreeType::SumTree.inner_node_type().cost())
                    .checked_add(
                        *big_sum_trees_weight as u32
                            * TreeType::BigSumTree.inner_node_type().cost(),
                    )
                    .and_then(|sum| {
                        sum.checked_add(
                            *count_trees_weight as u32
                                * TreeType::CountTree.inner_node_type().cost(),
                        )
                    })
                    .and_then(|sum| {
                        sum.checked_add(
                            *count_sum_trees_weight as u32
                                * TreeType::CountSumTree.inner_node_type().cost(),
                        )
                    })
                    .ok_or(Error::Overflow("Estimated size calculation overflowed"))?;

                    Ok(estimated_size / total_weight)
                } else {
                    Err(Error::CorruptedCodeExecution("we already checked versions"))
                }
            }
            EstimatedSumTrees::AllSumTrees => Ok(TreeType::SumTree.inner_node_type().cost()),
            EstimatedSumTrees::AllBigSumTrees => Ok(TreeType::BigSumTree.inner_node_type().cost()),
            EstimatedSumTrees::AllCountTrees => Ok(TreeType::CountTree.inner_node_type().cost()),
            EstimatedSumTrees::AllCountSumTrees => {
                Ok(TreeType::CountSumTree.inner_node_type().cost())
            }
        }
    }
}

#[cfg(feature = "minimal")]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
/// Estimated layer sizes
pub enum EstimatedLayerSizes {
    /// All subtrees
    AllSubtrees(AverageKeySize, EstimatedSumTrees, Option<AverageFlagsSize>),
    /// All items
    AllItems(AverageKeySize, AverageValueSize, Option<AverageFlagsSize>),
    /// References
    AllReference(AverageKeySize, AverageValueSize, Option<AverageFlagsSize>),
    /// Mix
    Mix {
        /// Subtrees size
        subtrees_size: Option<(
            AverageKeySize,
            EstimatedSumTrees,
            Option<AverageFlagsSize>,
            Weight,
        )>,
        /// Items size
        items_size: Option<(
            AverageKeySize,
            AverageValueSize,
            Option<AverageFlagsSize>,
            Weight,
        )>,
        /// References size
        references_size: Option<(
            AverageKeySize,
            AverageValueSize,
            Option<AverageFlagsSize>,
            Weight,
        )>,
    },
}

#[cfg(feature = "minimal")]
impl EstimatedLayerSizes {
    /// Return average flags size for layer
    pub fn layered_flags_size(&self) -> Result<&Option<AverageFlagsSize>, Error> {
        match self {
            EstimatedLayerSizes::AllSubtrees(_, _, flags_size) => Ok(flags_size),
            EstimatedLayerSizes::Mix {
                subtrees_size: subtree_size,
                items_size: _,
                references_size: _,
            } => {
                if let Some((_, _, flags_size, _)) = subtree_size {
                    Ok(flags_size)
                } else {
                    Err(Error::WrongEstimatedCostsElementTypeForLevel(
                        "this mixed layer does not have costs for trees",
                    ))
                }
            }
            _ => Err(Error::WrongEstimatedCostsElementTypeForLevel(
                "this layer does not have costs for trees",
            )),
        }
    }

    /// Returns the size of a subtree's feature and flags
    /// This only takes into account subtrees in the estimated layer info
    /// Only should be used when it is known to be a subtree
    pub fn subtree_with_feature_and_flags_size(
        &self,
        grove_version: &GroveVersion,
    ) -> Result<u32, Error> {
        match self {
            EstimatedLayerSizes::AllSubtrees(_, estimated_sum_trees, flags_size) => {
                // 1 for enum type
                // 1 for empty
                // 1 for flags size
                Ok(estimated_sum_trees.estimated_size(grove_version)?
                    + flags_size.unwrap_or_default()
                    + 3)
            }
            EstimatedLayerSizes::Mix { subtrees_size, .. } => match subtrees_size {
                None => Err(Error::WrongEstimatedCostsElementTypeForLevel(
                    "this layer is a mix but doesn't have subtrees",
                )),
                Some((_, est, fs, _)) => {
                    Ok(est.estimated_size(grove_version)? + fs.unwrap_or_default() + 3)
                }
            },
            _ => Err(Error::WrongEstimatedCostsElementTypeForLevel(
                "this layer needs to have trees",
            )),
        }
    }

    /// Returns the size of a value's feature and flags
    pub fn value_with_feature_and_flags_size(
        &self,
        grove_version: &GroveVersion,
    ) -> Result<u32, Error> {
        match self {
            EstimatedLayerSizes::AllItems(_, average_value_size, flags_size) => {
                // 1 for enum type
                // 1 for value size
                // 1 for flags size
                Ok(*average_value_size + flags_size.unwrap_or_default() + 3)
            }
            EstimatedLayerSizes::AllReference(_, average_value_size, flags_size) => {
                // 1 for enum type
                // 1 for value size
                // 1 for flags size
                // 2 for reference hops
                Ok(*average_value_size + flags_size.unwrap_or_default() + 5)
            }
            EstimatedLayerSizes::AllSubtrees(_, estimated_sum_trees, flags_size) => {
                // 1 for enum type
                // 1 for empty
                // 1 for flags size
                Ok(estimated_sum_trees.estimated_size(grove_version)?
                    + flags_size.unwrap_or_default()
                    + 3)
            }
            EstimatedLayerSizes::Mix {
                subtrees_size,
                items_size,
                references_size,
            } => {
                let (item_size, item_weight) = items_size
                    .as_ref()
                    .map(|(_, vs, fs, weight)| (vs + fs.unwrap_or_default() + 3, *weight as u32))
                    .unwrap_or_default();

                let (ref_size, ref_weight) = references_size
                    .as_ref()
                    .map(|(_, vs, fs, weight)| (vs + fs.unwrap_or_default() + 5, *weight as u32))
                    .unwrap_or_default();

                let (subtree_size, subtree_weight) = match subtrees_size {
                    None => None,
                    Some((_, est, fs, weight)) => Some((
                        est.estimated_size(grove_version)? + fs.unwrap_or_default() + 3,
                        *weight as u32,
                    )),
                }
                .unwrap_or_default();

                if item_weight == 0 && ref_weight == 0 && subtree_weight == 0 {
                    return Err(Error::WrongEstimatedCostsElementTypeForLevel(
                        "this layer is a mix and does not have items, refs or trees",
                    ));
                }
                if item_weight == 0 && ref_weight == 0 {
                    return Ok(subtree_size);
                }
                if item_weight == 0 && subtree_weight == 0 {
                    return Ok(ref_size);
                }
                if ref_weight == 0 && subtree_weight == 0 {
                    return Ok(item_size);
                }
                let combined_weight = item_weight
                    .checked_add(ref_weight)
                    .and_then(|a| a.checked_add(subtree_weight))
                    .ok_or(Error::Overflow("overflow for value size combining weights"))?;
                item_size
                    .checked_add(ref_size)
                    .and_then(|a| a.checked_add(subtree_size))
                    .and_then(|a| a.checked_div(combined_weight))
                    .ok_or(Error::Overflow("overflow for value size"))
            }
        }
    }
}

#[cfg(feature = "minimal")]
/// Approximate element count
pub type ApproximateElementCount = u32;
#[cfg(feature = "minimal")]
/// Estimated level number
pub type EstimatedLevelNumber = u32;
#[cfg(feature = "minimal")]
/// Estimated to be empty
pub type EstimatedToBeEmpty = bool;

#[cfg(feature = "minimal")]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
/// Information on an estimated layer
pub struct EstimatedLayerInformation {
    /// The kind of tree we are in
    pub tree_type: TreeType,
    /// Estimated layer count
    pub estimated_layer_count: EstimatedLayerCount,
    /// Estimated layer sizes
    pub estimated_layer_sizes: EstimatedLayerSizes,
}

#[cfg(feature = "minimal")]
impl EstimatedLayerInformation {}

#[cfg(feature = "minimal")]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
/// Estimated elements and level number of a layer
pub enum EstimatedLayerCount {
    /// Potentially at max elements
    PotentiallyAtMaxElements,
    /// Approximate elements
    ApproximateElements(ApproximateElementCount),
    /// Estimated level
    EstimatedLevel(EstimatedLevelNumber, EstimatedToBeEmpty),
}

#[cfg(feature = "minimal")]
impl EstimatedLayerCount {
    /// Returns true if the tree is estimated to be empty.
    pub fn estimated_to_be_empty(&self) -> bool {
        match self {
            EstimatedLayerCount::ApproximateElements(count) => *count == 0,
            EstimatedLayerCount::PotentiallyAtMaxElements => false,
            EstimatedLayerCount::EstimatedLevel(_, empty) => *empty,
        }
    }

    /// Estimate the number of levels based on the size of the tree, for big
    /// trees this is very inaccurate.
    pub fn estimate_levels(&self) -> u32 {
        match self {
            EstimatedLayerCount::ApproximateElements(n) => {
                if *n == u32::MAX {
                    32
                } else {
                    ((n + 1) as f32).log2().ceil() as u32
                }
            }
            EstimatedLayerCount::PotentiallyAtMaxElements => 32,
            EstimatedLayerCount::EstimatedLevel(n, _) => *n,
        }
    }
}

#[cfg(feature = "minimal")]
impl TreeNode {
    /// Return estimate of average encoded tree size
    pub fn average_case_encoded_tree_size(
        not_prefixed_key_len: u32,
        estimated_element_size: u32,
        node_type: NodeType,
    ) -> u32 {
        // two option values for the left and right link
        // the actual left and right link encoding size
        // the encoded kv node size
        2 + (2 * Link::encoded_link_size(not_prefixed_key_len, node_type))
            + KV::encoded_kv_node_size(estimated_element_size, node_type)
    }
}

#[cfg(feature = "minimal")]
/// Add worst case for getting a merk node
pub fn add_average_case_get_merk_node(
    cost: &mut OperationCost,
    not_prefixed_key_len: u32,
    approximate_element_size: u32,
    node_type: NodeType,
) -> Result<(), Error> {
    // Worst case scenario, the element is not already in memory.
    // One direct seek has to be performed to read the node from storage.
    cost.seek_count += 1;

    // To write a node to disk, the left link, right link and kv nodes are encoded.
    // worst case, the node has both the left and right link present.
    cost.storage_loaded_bytes += TreeNode::average_case_encoded_tree_size(
        not_prefixed_key_len,
        approximate_element_size,
        node_type,
    ) as u64;
    Ok(())
}

#[cfg(feature = "minimal")]
/// Add worst case for getting a merk tree
pub fn add_average_case_merk_has_value(
    cost: &mut OperationCost,
    not_prefixed_key_len: u32,
    estimated_element_size: u32,
) {
    cost.seek_count += 1;
    cost.storage_loaded_bytes += (not_prefixed_key_len + estimated_element_size) as u64;
}

#[cfg(feature = "minimal")]
/// Add worst case for insertion into merk
pub fn add_average_case_merk_replace_layered(
    cost: &mut OperationCost,
    key_len: u32,
    value_len: u32,
    node_type: NodeType,
) {
    cost.seek_count += 1;
    cost.storage_cost.replaced_bytes =
        KV::layered_value_byte_cost_size_for_key_and_value_lengths(key_len, value_len, node_type);

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
/// Add average case for deletion from merk
pub fn add_average_case_merk_delete_layered(
    cost: &mut OperationCost,
    _key_len: u32,
    value_len: u32,
) {
    // todo: verify this
    cost.seek_count += 1;
    cost.hash_node_calls += 1 + ((value_len - 1) / HASH_BLOCK_SIZE_U32);
}

#[cfg(feature = "minimal")]
/// Add average case for deletion from merk
pub fn add_average_case_merk_delete(cost: &mut OperationCost, _key_len: u32, value_len: u32) {
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
pub fn add_average_case_merk_root_hash(cost: &mut OperationCost) {
    cost.hash_node_calls += node_hash_update_count();
}

#[cfg(feature = "minimal")]
/// Average case cost of propagating a merk
pub fn average_case_merk_propagate(
    input: &EstimatedLayerInformation,
    grove_version: &GroveVersion,
) -> CostResult<(), Error> {
    let mut cost = OperationCost::default();
    add_average_case_merk_propagate(&mut cost, input, grove_version).wrap_with_cost(cost)
}

#[cfg(feature = "minimal")]
/// Add average case cost for propagating a merk
pub fn add_average_case_merk_propagate(
    cost: &mut OperationCost,
    input: &EstimatedLayerInformation,
    grove_version: &GroveVersion,
) -> Result<(), Error> {
    match grove_version
        .merk_versions
        .average_case_costs
        .add_average_case_merk_propagate
    {
        0 => add_average_case_merk_propagate_v0(cost, input, grove_version),
        1 => add_average_case_merk_propagate_v1(cost, input, grove_version),
        version => Err(Error::VersionError(
            GroveVersionError::UnknownVersionMismatch {
                method: "add_average_case_merk_propagate".to_string(),
                known_versions: vec![0, 1],
                received: version,
            },
        )),
    }
}
#[cfg(feature = "minimal")]
/// Add average case cost for propagating a merk
fn add_average_case_merk_propagate_v1(
    cost: &mut OperationCost,
    input: &EstimatedLayerInformation,
    grove_version: &GroveVersion,
) -> Result<(), Error> {
    let mut nodes_updated = 0;
    // Propagation requires to recompute and write hashes up to the root
    let EstimatedLayerInformation {
        tree_type,
        estimated_layer_count,
        estimated_layer_sizes,
    } = input;
    let levels = estimated_layer_count.estimate_levels();
    nodes_updated += levels;

    if levels > 1 {
        // we can get about 1 rotation, if there are more than 2 levels
        nodes_updated += 1;
    }
    cost.seek_count += nodes_updated;

    cost.hash_node_calls += nodes_updated * 2;

    cost.storage_cost.replaced_bytes += match estimated_layer_sizes {
        EstimatedLayerSizes::AllSubtrees(
            average_key_size,
            estimated_sum_trees,
            average_flags_size,
        ) => {
            // it is normal to have LAYER_COST_SIZE here, as we add estimated sum tree
            // additions right after
            let value_len = LAYER_COST_SIZE
                + average_flags_size
                    .map_or(0, |flags_len| flags_len + flags_len.required_space() as u32);
            // in order to simplify calculations we get the estimated size and remove the
            // cost for the basic merk
            let sum_tree_addition = estimated_sum_trees.estimated_size(grove_version)?;
            nodes_updated
                * (KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    *average_key_size as u32,
                    value_len,
                    tree_type.inner_node_type(),
                ) + sum_tree_addition)
        }
        EstimatedLayerSizes::AllItems(average_key_size, average_item_size, average_flags_size)
        | EstimatedLayerSizes::AllReference(
            average_key_size,
            average_item_size,
            average_flags_size,
        ) => {
            let flags_len = average_flags_size.unwrap_or(0);
            let average_value_len = average_item_size + flags_len;
            nodes_updated
                * KV::value_byte_cost_size_for_key_and_raw_value_lengths(
                    *average_key_size as u32,
                    average_value_len,
                    tree_type.inner_node_type(),
                )
        }
        EstimatedLayerSizes::Mix {
            subtrees_size,
            items_size,
            references_size,
        } => {
            let total_weight = subtrees_size
                .as_ref()
                .map(|(_, _, _, weight)| *weight as u32)
                .unwrap_or_default()
                + items_size
                    .as_ref()
                    .map(|(_, _, _, weight)| *weight as u32)
                    .unwrap_or_default()
                + references_size
                    .as_ref()
                    .map(|(_, _, _, weight)| *weight as u32)
                    .unwrap_or_default();
            if total_weight == 0 {
                0
            } else {
                let weighted_nodes_updated = (nodes_updated as u64)
                    .checked_mul(total_weight as u64)
                    .ok_or(Error::Overflow("overflow for weights average cost"))?;
                let tree_node_updates_cost = match subtrees_size {
                    None => 0,
                    Some((average_key_size, estimated_sum_trees, average_flags_size, weight)) => {
                        let flags_len = average_flags_size.unwrap_or(0);
                        let value_len = LAYER_COST_SIZE + flags_len;
                        let sum_tree_addition =
                            estimated_sum_trees.estimated_size(grove_version)?;
                        let cost = KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                            *average_key_size as u32,
                            value_len,
                            tree_type.inner_node_type(),
                        ) + sum_tree_addition;
                        (*weight as u64)
                            .checked_mul(cost as u64)
                            .ok_or(Error::Overflow("overflow for mixed tree nodes updates"))?
                    }
                };
                let item_node_updates_cost = match items_size {
                    None => 0,
                    Some((average_key_size, average_value_size, average_flags_size, weight)) => {
                        let flags_len = average_flags_size.unwrap_or(0);
                        let value_len = average_value_size + flags_len;
                        let cost = KV::value_byte_cost_size_for_key_and_raw_value_lengths(
                            *average_key_size as u32,
                            value_len,
                            tree_type.inner_node_type(),
                        );
                        (*weight as u64)
                            .checked_mul(cost as u64)
                            .ok_or(Error::Overflow("overflow for mixed item nodes updates"))?
                    }
                };
                let reference_node_updates_cost = match references_size {
                    None => 0,
                    Some((average_key_size, average_value_size, average_flags_size, weight)) => {
                        let flags_len = average_flags_size.unwrap_or(0);
                        let value_len = average_value_size + flags_len;
                        let cost = KV::value_byte_cost_size_for_key_and_raw_value_lengths(
                            *average_key_size as u32,
                            value_len,
                            tree_type.inner_node_type(),
                        );
                        (*weight as u64)
                            .checked_mul(cost as u64)
                            .ok_or(Error::Overflow("overflow for mixed item nodes updates"))?
                    }
                };

                let total_updates_cost = tree_node_updates_cost
                    .checked_add(item_node_updates_cost)
                    .and_then(|c| c.checked_add(reference_node_updates_cost))
                    .ok_or(Error::Overflow("overflow for mixed item adding parts"))?;
                let total_replaced_bytes = total_updates_cost / weighted_nodes_updated;
                if total_replaced_bytes > u32::MAX as u64 {
                    return Err(Error::Overflow(
                        "overflow for total replaced bytes more than u32 max",
                    ));
                }
                total_replaced_bytes as u32
            }
        }
    };
    cost.storage_loaded_bytes += match estimated_layer_sizes {
        EstimatedLayerSizes::AllSubtrees(
            average_key_size,
            estimated_sum_trees,
            average_flags_size,
        ) => {
            let flags_len = average_flags_size.unwrap_or(0);
            let value_len = LAYER_COST_SIZE + flags_len;
            let sum_tree_addition = estimated_sum_trees.estimated_size(grove_version)?;
            nodes_updated
                * KV::layered_node_byte_cost_size_for_key_and_value_lengths(
                    *average_key_size as u32,
                    value_len + sum_tree_addition,
                    tree_type.inner_node_type(),
                )
        }
        EstimatedLayerSizes::AllItems(average_key_size, average_item_size, average_flags_size)
        | EstimatedLayerSizes::AllReference(
            average_key_size,
            average_item_size,
            average_flags_size,
        ) => {
            let flags_len = average_flags_size.unwrap_or(0);
            let average_value_len = average_item_size + flags_len;
            nodes_updated
                * KV::node_byte_cost_size_for_key_and_raw_value_lengths(
                    *average_key_size as u32,
                    average_value_len,
                    tree_type.inner_node_type(),
                )
        }
        EstimatedLayerSizes::Mix {
            subtrees_size,
            items_size,
            references_size,
        } => {
            let total_weight = subtrees_size
                .as_ref()
                .map(|(_, _, _, weight)| *weight as u32)
                .unwrap_or_default()
                + items_size
                    .as_ref()
                    .map(|(_, _, _, weight)| *weight as u32)
                    .unwrap_or_default()
                + references_size
                    .as_ref()
                    .map(|(_, _, _, weight)| *weight as u32)
                    .unwrap_or_default();
            if total_weight == 0 {
                0
            } else {
                let weighted_nodes_updated = (nodes_updated as u64)
                    .checked_mul(total_weight as u64)
                    .ok_or(Error::Overflow("overflow for weights average cost"))?;
                let tree_node_updates_cost = subtrees_size
                    .as_ref()
                    .map(
                        |(average_key_size, estimated_sum_trees, average_flags_size, weight)| {
                            let flags_len = average_flags_size.unwrap_or(0);
                            let value_len = LAYER_COST_SIZE + flags_len;
                            let sum_tree_addition =
                                estimated_sum_trees.estimated_size(grove_version)?;
                            let cost = KV::layered_node_byte_cost_size_for_key_and_value_lengths(
                                *average_key_size as u32,
                                value_len + sum_tree_addition,
                                tree_type.inner_node_type(),
                            );
                            (*weight as u64)
                                .checked_mul(cost as u64)
                                .ok_or(Error::Overflow("overflow for mixed tree nodes updates"))
                        },
                    )
                    .unwrap_or(Ok(0))?;
                let item_node_updates_cost = items_size
                    .as_ref()
                    .map(
                        |(average_key_size, average_value_size, average_flags_size, weight)| {
                            let flags_len = average_flags_size.unwrap_or(0);
                            let value_len = average_value_size + flags_len;
                            let cost = KV::node_byte_cost_size_for_key_and_raw_value_lengths(
                                *average_key_size as u32,
                                value_len,
                                tree_type.inner_node_type(),
                            );
                            (*weight as u64)
                                .checked_mul(cost as u64)
                                .ok_or(Error::Overflow("overflow for mixed item nodes updates"))
                        },
                    )
                    .unwrap_or(Ok(0))?;
                let reference_node_updates_cost = references_size
                    .as_ref()
                    .map(
                        |(average_key_size, average_value_size, average_flags_size, weight)| {
                            let flags_len = average_flags_size.unwrap_or(0);
                            let value_len = average_value_size + flags_len;
                            let cost = KV::node_byte_cost_size_for_key_and_raw_value_lengths(
                                *average_key_size as u32,
                                value_len,
                                TreeType::NormalTree.inner_node_type(),
                            );
                            (*weight as u64)
                                .checked_mul(cost as u64)
                                .ok_or(Error::Overflow("overflow for mixed item nodes updates"))
                        },
                    )
                    .unwrap_or(Ok(0))?;

                let total_updates_cost = tree_node_updates_cost
                    .checked_add(item_node_updates_cost)
                    .and_then(|c| c.checked_add(reference_node_updates_cost))
                    .ok_or(Error::Overflow("overflow for mixed item adding parts"))?;
                let total_loaded_bytes = total_updates_cost / weighted_nodes_updated;
                if total_loaded_bytes > u32::MAX as u64 {
                    return Err(Error::Overflow(
                        "overflow for total replaced bytes more than u32 max",
                    ));
                }
                total_loaded_bytes as u32
            }
        }
    } as u64;
    Ok(())
}

#[cfg(feature = "minimal")]
/// Add average case cost for propagating a merk
fn add_average_case_merk_propagate_v0(
    cost: &mut OperationCost,
    input: &EstimatedLayerInformation,
    grove_version: &GroveVersion,
) -> Result<(), Error> {
    let mut nodes_updated = 0;
    // Propagation requires to recompute and write hashes up to the root
    let EstimatedLayerInformation {
        tree_type,
        estimated_layer_count,
        estimated_layer_sizes,
    } = input;
    let levels = estimated_layer_count.estimate_levels();
    nodes_updated += levels;

    if levels > 1 {
        // we can get about 1 rotation, if there are more than 2 levels
        nodes_updated += 1;
    }
    cost.seek_count += nodes_updated;

    cost.hash_node_calls += nodes_updated * 2;

    cost.storage_cost.replaced_bytes += match estimated_layer_sizes {
        EstimatedLayerSizes::AllSubtrees(
            average_key_size,
            estimated_sum_trees,
            average_flags_size,
        ) => {
            // it is normal to have LAYER_COST_SIZE here, as we add estimated sum tree
            // additions right after
            let value_len = LAYER_COST_SIZE
                + average_flags_size
                    .map_or(0, |flags_len| flags_len + flags_len.required_space() as u32);
            // in order to simplify calculations we get the estimated size and remove the
            // cost for the basic merk
            let sum_tree_addition = estimated_sum_trees.estimated_size(grove_version)?;
            nodes_updated
                * (KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    *average_key_size as u32,
                    value_len,
                    tree_type.inner_node_type(),
                ) + sum_tree_addition)
        }
        EstimatedLayerSizes::AllItems(average_key_size, average_item_size, average_flags_size)
        | EstimatedLayerSizes::AllReference(
            average_key_size,
            average_item_size,
            average_flags_size,
        ) => {
            let flags_len = average_flags_size.unwrap_or(0);
            let average_value_len = average_item_size + flags_len;
            nodes_updated
                * KV::value_byte_cost_size_for_key_and_raw_value_lengths(
                    *average_key_size as u32,
                    average_value_len,
                    tree_type.inner_node_type(),
                )
        }
        EstimatedLayerSizes::Mix {
            subtrees_size,
            items_size,
            references_size,
        } => {
            let total_weight = subtrees_size
                .as_ref()
                .map(|(_, _, _, weight)| *weight as u32)
                .unwrap_or_default()
                + items_size
                    .as_ref()
                    .map(|(_, _, _, weight)| *weight as u32)
                    .unwrap_or_default()
                + references_size
                    .as_ref()
                    .map(|(_, _, _, weight)| *weight as u32)
                    .unwrap_or_default();
            if total_weight == 0 {
                0
            } else {
                let weighted_nodes_updated = (nodes_updated as u64)
                    .checked_mul(total_weight as u64)
                    .ok_or(Error::Overflow("overflow for weights average cost"))?;
                let tree_node_updates_cost = match subtrees_size {
                    None => 0,
                    Some((average_key_size, estimated_sum_trees, average_flags_size, weight)) => {
                        let flags_len = average_flags_size.unwrap_or(0);
                        let value_len = LAYER_COST_SIZE + flags_len;
                        let sum_tree_addition =
                            estimated_sum_trees.estimated_size(grove_version)?;
                        let cost = KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                            *average_key_size as u32,
                            value_len,
                            tree_type.inner_node_type(),
                        ) + sum_tree_addition;
                        (*weight as u64)
                            .checked_mul(cost as u64)
                            .ok_or(Error::Overflow("overflow for mixed tree nodes updates"))?
                    }
                };
                let item_node_updates_cost = match items_size {
                    None => 0,
                    Some((average_key_size, average_value_size, average_flags_size, weight)) => {
                        let flags_len = average_flags_size.unwrap_or(0);
                        let value_len = average_value_size + flags_len;
                        let cost = KV::value_byte_cost_size_for_key_and_raw_value_lengths(
                            *average_key_size as u32,
                            value_len,
                            tree_type.inner_node_type(),
                        );
                        (*weight as u64)
                            .checked_mul(cost as u64)
                            .ok_or(Error::Overflow("overflow for mixed item nodes updates"))?
                    }
                };
                let reference_node_updates_cost = match references_size {
                    None => 0,
                    Some((average_key_size, average_value_size, average_flags_size, weight)) => {
                        let flags_len = average_flags_size.unwrap_or(0);
                        let value_len = average_value_size + flags_len;
                        let cost = KV::value_byte_cost_size_for_key_and_raw_value_lengths(
                            *average_key_size as u32,
                            value_len,
                            tree_type.inner_node_type(),
                        );
                        (*weight as u64)
                            .checked_mul(cost as u64)
                            .ok_or(Error::Overflow("overflow for mixed item nodes updates"))?
                    }
                };

                let total_updates_cost = tree_node_updates_cost
                    .checked_add(item_node_updates_cost)
                    .and_then(|c| c.checked_add(reference_node_updates_cost))
                    .ok_or(Error::Overflow("overflow for mixed item adding parts"))?;
                let total_replaced_bytes = total_updates_cost / weighted_nodes_updated;
                if total_replaced_bytes > u32::MAX as u64 {
                    return Err(Error::Overflow(
                        "overflow for total replaced bytes more than u32 max",
                    ));
                }
                total_replaced_bytes as u32
            }
        }
    };
    cost.storage_loaded_bytes += match estimated_layer_sizes {
        EstimatedLayerSizes::AllSubtrees(
            average_key_size,
            estimated_sum_trees,
            average_flags_size,
        ) => {
            let flags_len = average_flags_size.unwrap_or(0);
            let value_len = LAYER_COST_SIZE + flags_len;
            let sum_tree_addition = estimated_sum_trees.estimated_size(grove_version)?;
            nodes_updated
                * KV::layered_node_byte_cost_size_for_key_and_value_lengths(
                    *average_key_size as u32,
                    value_len + sum_tree_addition,
                    tree_type.inner_node_type(),
                )
        }
        EstimatedLayerSizes::AllItems(average_key_size, average_item_size, average_flags_size)
        | EstimatedLayerSizes::AllReference(
            average_key_size,
            average_item_size,
            average_flags_size,
        ) => {
            let flags_len = average_flags_size.unwrap_or(0);
            let average_value_len = average_item_size + flags_len;
            nodes_updated
                * KV::node_byte_cost_size_for_key_and_raw_value_lengths(
                    *average_key_size as u32,
                    average_value_len,
                    tree_type.inner_node_type(),
                )
        }
        EstimatedLayerSizes::Mix {
            subtrees_size,
            items_size,
            references_size,
        } => {
            let total_weight = subtrees_size
                .as_ref()
                .map(|(_, _, _, weight)| *weight as u32)
                .unwrap_or_default()
                + items_size
                    .as_ref()
                    .map(|(_, _, _, weight)| *weight as u32)
                    .unwrap_or_default()
                + references_size
                    .as_ref()
                    .map(|(_, _, _, weight)| *weight as u32)
                    .unwrap_or_default();
            if total_weight == 0 {
                0
            } else {
                let weighted_nodes_updated = (nodes_updated as u64)
                    .checked_mul(total_weight as u64)
                    .ok_or(Error::Overflow("overflow for weights average cost"))?;
                let tree_node_updates_cost = subtrees_size
                    .as_ref()
                    .map(
                        |(average_key_size, estimated_sum_trees, average_flags_size, weight)| {
                            let flags_len = average_flags_size.unwrap_or(0);
                            let value_len = LAYER_COST_SIZE + flags_len;
                            let sum_tree_addition =
                                estimated_sum_trees.estimated_size(grove_version)?;
                            let cost = KV::layered_node_byte_cost_size_for_key_and_value_lengths(
                                *average_key_size as u32,
                                value_len + sum_tree_addition,
                                tree_type.inner_node_type(),
                            );
                            (*weight as u64)
                                .checked_mul(cost as u64)
                                .ok_or(Error::Overflow("overflow for mixed tree nodes updates"))
                        },
                    )
                    .unwrap_or(Ok(0))?;
                let item_node_updates_cost = items_size
                    .as_ref()
                    .map(
                        |(average_key_size, average_value_size, average_flags_size, weight)| {
                            let flags_len = average_flags_size.unwrap_or(0);
                            let value_len = average_value_size + flags_len;
                            let cost = KV::node_byte_cost_size_for_key_and_raw_value_lengths(
                                *average_key_size as u32,
                                value_len,
                                tree_type.inner_node_type(),
                            );
                            (*weight as u64)
                                .checked_mul(cost as u64)
                                .ok_or(Error::Overflow("overflow for mixed item nodes updates"))
                        },
                    )
                    .unwrap_or(Ok(0))?;
                let reference_node_updates_cost = references_size
                    .as_ref()
                    .map(
                        |(average_key_size, average_value_size, average_flags_size, weight)| {
                            let flags_len = average_flags_size.unwrap_or(0);
                            let value_len = average_value_size + flags_len;
                            let cost = KV::node_byte_cost_size_for_key_and_raw_value_lengths(
                                *average_key_size as u32,
                                value_len,
                                tree_type.inner_node_type(), // this was changed in v1
                            );
                            (*weight as u64)
                                .checked_mul(cost as u64)
                                .ok_or(Error::Overflow("overflow for mixed item nodes updates"))
                        },
                    )
                    .unwrap_or(Ok(0))?;

                let total_updates_cost = tree_node_updates_cost
                    .checked_add(item_node_updates_cost)
                    .and_then(|c| c.checked_add(reference_node_updates_cost))
                    .ok_or(Error::Overflow("overflow for mixed item adding parts"))?;
                let total_loaded_bytes = total_updates_cost / weighted_nodes_updated;
                if total_loaded_bytes > u32::MAX as u64 {
                    return Err(Error::Overflow(
                        "overflow for total replaced bytes more than u32 max",
                    ));
                }
                total_loaded_bytes as u32
            }
        }
    } as u64;
    Ok(())
}
