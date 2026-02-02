use grovedb_costs::storage_cost::removal::{
    StorageRemovedBytes, StorageRemovedBytes::BasicStorageRemoval,
};

use crate::{error::StorageFlagsError, ElementFlags, StorageFlags};

impl StorageFlags {
    pub fn split_removal_bytes(
        flags: &mut ElementFlags,
        removed_key_bytes: u32,
        removed_value_bytes: u32,
    ) -> Result<(StorageRemovedBytes, StorageRemovedBytes), StorageFlagsError> {
        let maybe_storage_flags =
            StorageFlags::from_element_flags_ref(flags).map_err(|mut e| {
                e.add_info("drive did not understand flags of item being updated");
                e
            })?;
        // if we removed key bytes then we removed the entire value
        match maybe_storage_flags {
            None => Ok((
                BasicStorageRemoval(removed_key_bytes),
                BasicStorageRemoval(removed_value_bytes),
            )),
            Some(storage_flags) => {
                Ok(storage_flags
                    .split_storage_removed_bytes(removed_key_bytes, removed_value_bytes))
            }
        }
    }
}
