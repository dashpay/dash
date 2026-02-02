use grovedb_costs::storage_cost::{transition::OperationStorageTransitionType, StorageCost};

use crate::{error::StorageFlagsError, ElementFlags, MergingOwnersStrategy, StorageFlags};

impl StorageFlags {
    pub fn update_element_flags(
        cost: &StorageCost,
        old_flags: Option<ElementFlags>,
        new_flags: &mut ElementFlags,
    ) -> Result<bool, StorageFlagsError> {
        // if there were no flags before then the new flags are used
        let Some(old_flags) = old_flags else {
            return Ok(false);
        };

        // This could be none only because the old element didn't exist
        // If they were empty we get an error
        let maybe_old_storage_flags =
            StorageFlags::from_element_flags_ref(&old_flags).map_err(|mut e| {
                e.add_info("drive did not understand flags of old item being updated");
                e
            })?;
        let new_storage_flags = StorageFlags::from_element_flags_ref(new_flags)
            .map_err(|mut e| {
                e.add_info("drive did not understand updated item flag information");
                e
            })?
            .ok_or(StorageFlagsError::RemovingFlagsError(
                "removing flags from an item with flags is not allowed".to_string(),
            ))?;
        let binding = maybe_old_storage_flags.clone().unwrap();
        let old_epoch_index_map = binding.epoch_index_map();
        let new_epoch_index_map = new_storage_flags.epoch_index_map();
        if old_epoch_index_map.is_some() || new_epoch_index_map.is_some() {
            // println!("> old:{:?} new:{:?}", old_epoch_index_map,
            // new_epoch_index_map);
        }

        match &cost.transition_type() {
            OperationStorageTransitionType::OperationUpdateBiggerSize => {
                // In the case that the owners do not match up this means that there has been a
                // transfer  of ownership of the underlying document, the value
                // held is transferred to the new owner
                // println!(">---------------------combine_added_bytes:{}", cost.added_bytes);
                // println!(">---------------------apply_batch_with_add_costs old_flags:{:?}
                // new_flags:{:?}", maybe_old_storage_flags, new_storage_flags);
                let combined_storage_flags = StorageFlags::optional_combine_added_bytes(
                    maybe_old_storage_flags.clone(),
                    new_storage_flags.clone(),
                    cost.added_bytes,
                    MergingOwnersStrategy::UseTheirs,
                )
                .map_err(|mut e| {
                    e.add_info("drive could not combine storage flags (new flags were bigger)");
                    e
                })?;
                // println!(
                //     ">added_bytes:{} old:{} new:{} --> combined:{}",
                //     cost.added_bytes,
                //     if maybe_old_storage_flags.is_some() {
                //         maybe_old_storage_flags.as_ref().unwrap().to_string()
                //     } else {
                //         "None".to_string()
                //     },
                //     new_storage_flags,
                //     combined_storage_flags
                // );
                // if combined_storage_flags.epoch_index_map().is_some() {
                //     //println!("     --------> bigger_combined_flags:{:?}",
                // combined_storage_flags.epoch_index_map()); }
                let combined_flags = combined_storage_flags.to_element_flags();
                // it's possible they got bigger in the same epoch
                if combined_flags == *new_flags {
                    // they are the same there was no update
                    Ok(false)
                } else {
                    *new_flags = combined_flags;
                    Ok(true)
                }
            }
            OperationStorageTransitionType::OperationUpdateSmallerSize => {
                // println!(
                //     ">removing_bytes:{:?} old:{} new:{}",
                //     cost.removed_bytes,
                //     if maybe_old_storage_flags.is_some() {
                //         maybe_old_storage_flags.as_ref().unwrap().to_string()
                //     } else {
                //         "None".to_string()
                //     },
                //     new_storage_flags,
                // );
                // In the case that the owners do not match up this means that there has been a
                // transfer  of ownership of the underlying document, the value
                // held is transferred to the new owner
                let combined_storage_flags = StorageFlags::optional_combine_removed_bytes(
                    maybe_old_storage_flags.clone(),
                    new_storage_flags.clone(),
                    &cost.removed_bytes,
                    MergingOwnersStrategy::UseTheirs,
                )
                .map_err(|mut e| {
                    e.add_info("drive could not combine storage flags (new flags were smaller)");
                    e
                })?;
                // println!(
                //     ">removed_bytes:{:?} old:{:?} new:{:?} --> combined:{:?}",
                //     cost.removed_bytes,
                //     maybe_old_storage_flags,
                //     new_storage_flags,
                //     combined_storage_flags
                // );
                if combined_storage_flags.epoch_index_map().is_some() {
                    // println!("     --------> smaller_combined_flags:{:?}",
                    // combined_storage_flags.epoch_index_map());
                }
                let combined_flags = combined_storage_flags.to_element_flags();
                // it's possible they got bigger in the same epoch
                if combined_flags == *new_flags {
                    // they are the same there was no update
                    Ok(false)
                } else {
                    *new_flags = combined_flags;
                    Ok(true)
                }
            }
            OperationStorageTransitionType::OperationUpdateSameSize => {
                if let Some(old_storage_flags) = maybe_old_storage_flags {
                    // if there were old storage flags we should just keep them
                    *new_flags = old_storage_flags.to_element_flags();
                    Ok(true)
                } else {
                    Ok(false)
                }
            }
            _ => Ok(false),
        }
    }
}
