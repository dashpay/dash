//! Average case delete cost

use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_no_add, CostResult, CostsExt, OperationCost,
};
use grovedb_merk::{
    estimated_costs::{
        average_case_costs::EstimatedLayerInformation,
        worst_case_costs::add_average_case_cost_for_is_empty_tree_except,
    },
    tree_type::TreeType,
    HASH_LENGTH_U32,
};
use grovedb_storage::{worst_case_costs::WorstKeyLength, Storage};
use grovedb_version::{check_grovedb_v0_with_cost, version::GroveVersion};
use intmap::IntMap;

use crate::{
    batch::{key_info::KeyInfo, KeyInfoPath, QualifiedGroveDbOp},
    Error, GroveDb,
};

/// 0 represents key size, 1 represents element size
type EstimatedKeyAndElementSize = (u32, u32);

impl GroveDb {
    /// Average case delete operations for delete up tree while empty
    // todo finish this
    pub fn average_case_delete_operations_for_delete_up_tree_while_empty<'db, S: Storage<'db>>(
        path: &KeyInfoPath,
        key: &KeyInfo,
        stop_path_height: Option<u16>,
        validate: bool,
        estimated_layer_info: IntMap<u16, EstimatedLayerInformation>,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<QualifiedGroveDbOp>, Error> {
        check_grovedb_v0_with_cost!(
            "average_case_delete_operations_for_delete_up_tree_while_empty",
            grove_version
                .grovedb_versions
                .operations
                .delete_up_tree
                .average_case_delete_operations_for_delete_up_tree_while_empty
        );
        let mut cost = OperationCost::default();

        let stop_path_height = stop_path_height.unwrap_or_default();

        if (path.len() as u16) < stop_path_height {
            // Attempt to delete a root tree leaf
            Err(Error::InvalidParameter(
                "path length need to be greater or equal to stop path height",
            ))
            .wrap_with_cost(cost)
        } else {
            let mut used_path = path.0.as_slice();
            let mut ops = vec![];
            let path_len = path.len() as u16;
            for height in (stop_path_height..path_len).rev() {
                let (
                    path_at_level,
                    key_at_level,
                    check_if_tree,
                    except_keys_count,
                    key_len,
                    estimated_element_size,
                    tree_type,
                ) = cost_return_on_error_no_add!(
                    cost,
                    if height == path_len - 1 {
                        if let Some(layer_info) = estimated_layer_info.get(height) {
                            let estimated_value_len = cost_return_on_error_no_add!(
                                cost,
                                layer_info
                                    .estimated_layer_sizes
                                    .value_with_feature_and_flags_size(grove_version)
                                    .map_err(Error::MerkError)
                            );
                            Ok((
                                used_path,
                                key,
                                true,
                                0,
                                key.max_length() as u32,
                                estimated_value_len,
                                layer_info.tree_type,
                            ))
                        } else {
                            Err(Error::InvalidParameter(
                                "intermediate flag size missing for height at path length",
                            ))
                        }
                    } else {
                        let (last_key, smaller_path) = used_path.split_last().unwrap();
                        used_path = smaller_path;
                        if let Some(layer_info) = estimated_layer_info.get(height) {
                            let estimated_value_len = cost_return_on_error_no_add!(
                                cost,
                                layer_info
                                    .estimated_layer_sizes
                                    .subtree_with_feature_and_flags_size(grove_version)
                                    .map_err(Error::MerkError)
                            );
                            Ok((
                                used_path,
                                last_key,
                                false,
                                1,
                                last_key.max_length() as u32,
                                estimated_value_len,
                                layer_info.tree_type,
                            ))
                        } else {
                            Err(Error::InvalidParameter("intermediate layer info missing"))
                        }
                    }
                );
                let op = cost_return_on_error!(
                    &mut cost,
                    Self::average_case_delete_operation_for_delete::<S>(
                        &KeyInfoPath::from_vec(path_at_level.to_vec()),
                        key_at_level,
                        tree_type,
                        validate,
                        check_if_tree,
                        except_keys_count,
                        (key_len, estimated_element_size),
                        grove_version,
                    )
                );
                ops.push(op);
            }
            Ok(ops).wrap_with_cost(cost)
        }
    }

    /// Average case delete operation for delete
    pub fn average_case_delete_operation_for_delete<'db, S: Storage<'db>>(
        path: &KeyInfoPath,
        key: &KeyInfo,
        in_parent_tree_type: TreeType,
        validate: bool,
        check_if_tree: bool,
        except_keys_count: u16,
        estimated_key_element_size: EstimatedKeyAndElementSize,
        grove_version: &GroveVersion,
    ) -> CostResult<QualifiedGroveDbOp, Error> {
        check_grovedb_v0_with_cost!(
            "average_case_delete_operation_for_delete",
            grove_version
                .grovedb_versions
                .operations
                .delete
                .average_case_delete_operation_for_delete
        );
        let mut cost = OperationCost::default();

        if validate {
            cost_return_on_error_no_add!(
                cost,
                GroveDb::add_average_case_get_merk_at_path::<S>(
                    &mut cost,
                    path,
                    false,
                    in_parent_tree_type,
                    grove_version,
                )
            );
        }
        if check_if_tree {
            cost_return_on_error_no_add!(
                cost,
                GroveDb::add_average_case_get_raw_cost::<S>(
                    &mut cost,
                    path,
                    key,
                    estimated_key_element_size.1,
                    in_parent_tree_type,
                    grove_version,
                )
            );
        }
        // in the worst case this is a tree
        add_average_case_cost_for_is_empty_tree_except(
            &mut cost,
            except_keys_count,
            estimated_key_element_size.0 + HASH_LENGTH_U32,
        );

        Ok(QualifiedGroveDbOp::delete_estimated_op(
            path.clone(),
            key.clone(),
        ))
        .wrap_with_cost(cost)
    }
}
