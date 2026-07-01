use std::borrow::Cow;

use grovedb_costs::{
    cost_return_on_error_into_no_add, cost_return_on_error_no_add,
    storage_cost::{
        removal::{StorageRemovedBytes, StorageRemovedBytes::BasicStorageRemoval},
        StorageCost,
    },
    CostResult, CostsExt, OperationCost,
};
use grovedb_merk::{
    tree::{kv::KV, value_hash, TreeNode},
    tree_type::TreeType,
    CryptoHash, Merk,
};
use grovedb_storage::StorageContext;
use grovedb_version::version::GroveVersion;

use crate::{
    batch::{MerkError, TreeCacheMerkByPath},
    Element, ElementFlags, Error,
};

impl<'db, S, F> TreeCacheMerkByPath<S, F>
where
    F: FnMut(&[Vec<u8>], bool) -> CostResult<Merk<S>, Error>,
    S: StorageContext<'db>,
{
    pub(crate) fn process_old_element_flags<G, SR>(
        key: &[u8],
        serialized: &[u8],
        new_element: &mut Element,
        old_element: Element,
        old_serialized_element: &[u8],
        in_tree_type: TreeType,
        flags_update: &mut G,
        split_removal_bytes: &mut SR,
        grove_version: &GroveVersion,
    ) -> CostResult<CryptoHash, Error>
    where
        G: FnMut(&StorageCost, Option<ElementFlags>, &mut ElementFlags) -> Result<bool, Error>,
        SR: FnMut(
            &mut ElementFlags,
            u32,
            u32,
        ) -> Result<(StorageRemovedBytes, StorageRemovedBytes), Error>,
    {
        let mut cost = OperationCost::default();
        if old_element.is_sum_item() {
            return if new_element.is_sum_item() {
                let maybe_old_flags = old_element.get_flags_owned();
                if maybe_old_flags.is_some() {
                    let mut updated_new_element_with_old_flags = new_element.clone();
                    updated_new_element_with_old_flags.set_flags(maybe_old_flags.clone());
                    // There are no storage flags, we can just hash new element
                    let new_serialized_bytes = cost_return_on_error_into_no_add!(
                        cost,
                        updated_new_element_with_old_flags.serialize(grove_version)
                    );
                    let val_hash = value_hash(&new_serialized_bytes).unwrap_add_cost(&mut cost);
                    Ok(val_hash).wrap_with_cost(cost)
                } else {
                    let val_hash = value_hash(serialized).unwrap_add_cost(&mut cost);
                    Ok(val_hash).wrap_with_cost(cost)
                }
            } else {
                Err(Error::NotSupported(
                    "going from a sum item to a not sum item is not supported".to_string(),
                ))
                .wrap_with_cost(cost)
            };
        } else if new_element.is_sum_item() {
            return Err(Error::NotSupported(
                "going from an item to a sum item is not supported".to_string(),
            ))
            .wrap_with_cost(cost);
        }
        let mut maybe_old_flags = old_element.get_flags_owned();

        let old_storage_cost = KV::node_value_byte_cost_size(
            key.len() as u32,
            old_serialized_element.len() as u32,
            in_tree_type.inner_node_type(),
        );

        let original_new_element = new_element.clone();

        let mut serialization_to_use = Cow::Borrowed(serialized);

        let mut new_storage_cost = if maybe_old_flags.is_some() {
            // we need to get the new storage_cost as if it had the same storage flags as
            // before
            let mut updated_new_element_with_old_flags = original_new_element.clone();
            updated_new_element_with_old_flags.set_flags(maybe_old_flags.clone());

            let serialized_with_old_flags = cost_return_on_error_into_no_add!(
                cost,
                updated_new_element_with_old_flags.serialize(grove_version)
            );
            KV::node_value_byte_cost_size(
                key.len() as u32,
                serialized_with_old_flags.len() as u32,
                in_tree_type.inner_node_type(),
            )
        } else {
            KV::node_value_byte_cost_size(
                key.len() as u32,
                serialized.len() as u32,
                in_tree_type.inner_node_type(),
            )
        };

        let mut i = 0;

        loop {
            // Calculate storage costs
            let mut storage_costs =
                TreeNode::storage_cost_for_update(new_storage_cost, old_storage_cost);

            if let Some(old_element_flags) = maybe_old_flags.as_mut() {
                if let BasicStorageRemoval(removed_bytes) = storage_costs.removed_bytes {
                    let (_, value_removed_bytes) = cost_return_on_error_no_add!(
                        cost,
                        split_removal_bytes(old_element_flags, 0, removed_bytes)
                    );
                    storage_costs.removed_bytes = value_removed_bytes;
                }
            }

            let mut new_element_cloned = original_new_element.clone();

            let changed = cost_return_on_error_no_add!(
                cost,
                (flags_update)(
                    &storage_costs,
                    maybe_old_flags.clone(),
                    new_element_cloned.get_flags_mut().as_mut().unwrap()
                )
                .map_err(|e| match e {
                    Error::JustInTimeElementFlagsClientError(_) => {
                        MerkError::ClientCorruptionError(e.to_string()).into()
                    }
                    _ => MerkError::ClientCorruptionError("non client error".to_string(),).into(),
                })
            );
            if !changed {
                // There are no storage flags, we can just hash new element

                let val_hash = value_hash(&serialization_to_use).unwrap_add_cost(&mut cost);
                return Ok(val_hash).wrap_with_cost(cost);
            } else {
                // There are no storage flags, we can just hash new element
                let new_serialized_bytes = cost_return_on_error_into_no_add!(
                    cost,
                    new_element_cloned.serialize(grove_version)
                );

                new_storage_cost = KV::node_value_byte_cost_size(
                    key.len() as u32,
                    new_serialized_bytes.len() as u32,
                    in_tree_type.inner_node_type(),
                );

                if serialization_to_use == new_serialized_bytes {
                    // it hasn't actually changed, let's do the value hash of it
                    let val_hash = value_hash(&serialization_to_use).unwrap_add_cost(&mut cost);
                    return Ok(val_hash).wrap_with_cost(cost);
                }

                serialization_to_use = Cow::Owned(new_serialized_bytes);
            }

            // Prevent potential infinite loop
            if i > 8 {
                return Err(Error::CyclicError(
                    "updated value based on costs too many times in reference",
                ))
                .wrap_with_cost(cost);
            }
            i += 1;
        }
    }
}
