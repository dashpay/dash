//! Worst case delete costs

use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_no_add, CostResult, CostsExt, OperationCost,
};
use grovedb_merk::{
    estimated_costs::worst_case_costs::add_worst_case_cost_for_is_empty_tree_except,
    tree::kv::KV,
    tree_type::{TreeType, SUM_TREE_COST_SIZE},
};
use grovedb_storage::{worst_case_costs::WorstKeyLength, Storage};
use grovedb_version::{check_grovedb_v0_with_cost, version::GroveVersion};
use intmap::IntMap;

use crate::{
    batch::{key_info::KeyInfo, KeyInfoPath, QualifiedGroveDbOp},
    Error, GroveDb,
};

#[cfg(feature = "minimal")]
impl GroveDb {
    /// Worst case costs for delete operations for delete up tree while empty
    pub fn worst_case_delete_operations_for_delete_up_tree_while_empty<'db, S: Storage<'db>>(
        path: &KeyInfoPath,
        key: &KeyInfo,
        stop_path_height: Option<u16>,
        validate: bool,
        intermediate_tree_info: IntMap<u64, (TreeType, u32)>,
        max_element_size: u32,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<QualifiedGroveDbOp>, Error> {
        check_grovedb_v0_with_cost!(
            "delete",
            grove_version
                .grovedb_versions
                .operations
                .delete_up_tree
                .worst_case_delete_operations_for_delete_up_tree_while_empty
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
                    max_element_size,
                    tree_type,
                ) = cost_return_on_error_no_add!(
                    cost,
                    if height == path_len {
                        if let Some((tree_type, _)) = intermediate_tree_info.get(height as u64) {
                            Ok((used_path, key, true, 0, max_element_size, *tree_type))
                        } else {
                            Err(Error::InvalidParameter(
                                "intermediate flag size missing for height at path length",
                            ))
                        }
                    } else {
                        let (last_key, smaller_path) = used_path.split_last().unwrap();
                        used_path = smaller_path;
                        if let Some((tree_type, flags_size_at_level)) =
                            intermediate_tree_info.get(height as u64)
                        {
                            // the worst case is that we are only in sum trees
                            // Todo the worst case is actually now big sum trees
                            let value_len = SUM_TREE_COST_SIZE + flags_size_at_level;
                            let max_tree_size =
                                KV::layered_node_byte_cost_size_for_key_and_value_lengths(
                                    last_key.max_length() as u32,
                                    value_len,
                                    tree_type.inner_node_type(),
                                );
                            Ok((used_path, last_key, false, 1, max_tree_size, *tree_type))
                        } else {
                            Err(Error::InvalidParameter("intermediate flag size missing"))
                        }
                    }
                );
                let op = cost_return_on_error!(
                    &mut cost,
                    Self::worst_case_delete_operation_for_delete::<S>(
                        &KeyInfoPath::from_vec(path_at_level.to_vec()),
                        key_at_level,
                        tree_type,
                        validate,
                        check_if_tree,
                        except_keys_count,
                        max_element_size,
                        grove_version
                    )
                );
                ops.push(op);
            }
            Ok(ops).wrap_with_cost(cost)
        }
    }

    /// Worst case costs for delete operation for delete
    pub fn worst_case_delete_operation_for_delete<'db, S: Storage<'db>>(
        path: &KeyInfoPath,
        key: &KeyInfo,
        in_parent_tree_type: TreeType,
        validate: bool,
        check_if_tree: bool,
        except_keys_count: u16,
        max_element_size: u32,
        grove_version: &GroveVersion,
    ) -> CostResult<QualifiedGroveDbOp, Error> {
        check_grovedb_v0_with_cost!(
            "worst_case_delete_operation_for_delete",
            grove_version
                .grovedb_versions
                .operations
                .delete
                .worst_case_delete_operation_for_delete
        );
        let mut cost = OperationCost::default();

        if validate {
            cost_return_on_error_no_add!(
                cost,
                GroveDb::add_worst_case_get_merk_at_path::<S>(
                    &mut cost,
                    path,
                    in_parent_tree_type,
                    grove_version,
                )
            );
        }
        if check_if_tree {
            cost_return_on_error_no_add!(
                cost,
                GroveDb::add_worst_case_get_raw_cost::<S>(
                    &mut cost,
                    path,
                    key,
                    max_element_size,
                    in_parent_tree_type,
                    grove_version,
                )
            );
        }
        // in the worst case this is a tree
        add_worst_case_cost_for_is_empty_tree_except(&mut cost, except_keys_count);

        Ok(QualifiedGroveDbOp::delete_estimated_op(
            path.clone(),
            key.clone(),
        ))
        .wrap_with_cost(cost)
    }
}
