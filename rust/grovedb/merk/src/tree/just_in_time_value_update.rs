use grovedb_costs::storage_cost::{
    removal::{StorageRemovedBytes, StorageRemovedBytes::BasicStorageRemoval},
    StorageCost,
};

use crate::{
    merk::defaults::MAX_UPDATE_VALUE_BASED_ON_COSTS_TIMES,
    tree::{kv::ValueDefinedCostType, TreeNode},
    Error,
};

impl TreeNode {
    pub(in crate::tree) fn just_in_time_tree_node_value_update(
        &mut self,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> Result<(), Error> {
        let mut i = 0;

        if let Some(old_value) = self.old_value.clone() {
            // At this point the tree value can be updated based on client requirements
            // For example to store the costs
            // todo: clean up clones
            let original_new_value = self.value_ref().clone();

            let new_value_with_old_flags = if self.inner.kv.value_defined_cost.is_none() {
                // for items
                get_temp_new_value_with_old_flags(&old_value, &original_new_value)?
            } else {
                // don't do this for sum items or trees
                None
            };

            let (mut current_tree_plus_hook_size, mut storage_costs) = self
                .kv_with_parent_hook_size_and_storage_cost_change_for_value(
                    old_specialized_cost,
                    new_value_with_old_flags,
                )?;

            loop {
                if let BasicStorageRemoval(removed_bytes) =
                    storage_costs.value_storage_cost.removed_bytes
                {
                    let (_, value_removed_bytes) =
                        section_removal_bytes(&old_value, 0, removed_bytes)?;
                    storage_costs.value_storage_cost.removed_bytes = value_removed_bytes;
                }

                let (flags_changed, value_defined_cost) = update_tree_value_based_on_costs(
                    &storage_costs.value_storage_cost,
                    &old_value,
                    self.value_mut_ref(),
                )?;
                if !flags_changed {
                    break;
                } else {
                    self.inner.kv.value_defined_cost = value_defined_cost;
                    let after_update_tree_plus_hook_size =
                        self.value_encoding_length_with_parent_to_child_reference();
                    if after_update_tree_plus_hook_size == current_tree_plus_hook_size {
                        break;
                    }
                    // we are calling this with merged flags that are were put in through value mut
                    // ref
                    let new_size_and_storage_costs =
                        self.kv_with_parent_hook_size_and_storage_cost(old_specialized_cost)?;
                    current_tree_plus_hook_size = new_size_and_storage_costs.0;
                    storage_costs = new_size_and_storage_costs.1;
                    self.set_value(original_new_value.clone())
                }
                if i > MAX_UPDATE_VALUE_BASED_ON_COSTS_TIMES {
                    return Err(Error::CyclicError(
                        "updated value based on costs too many times",
                    ));
                }
                i += 1;
            }

            if let BasicStorageRemoval(removed_bytes) =
                storage_costs.value_storage_cost.removed_bytes
            {
                let (_, value_removed_bytes) = section_removal_bytes(&old_value, 0, removed_bytes)?;
                storage_costs.value_storage_cost.removed_bytes = value_removed_bytes;
            }
            self.known_storage_cost = Some(storage_costs);
        } else {
            let (_, storage_costs) =
                self.kv_with_parent_hook_size_and_storage_cost(old_specialized_cost)?;
            self.known_storage_cost = Some(storage_costs);
        }

        self.old_value = Some(self.value_ref().clone());

        Ok(())
    }
}
