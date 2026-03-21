//! Get
//! Implements functions in Element for getting

use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_into_no_add, cost_return_on_error_no_add,
    CostResult, CostsExt, OperationCost,
};
use grovedb_element::{reference_path::util::path_as_slices_hex_to_ascii, Element};
use grovedb_storage::StorageContext;
use grovedb_version::{
    check_grovedb_v0_with_cost, error::GroveVersionError, version::GroveVersion,
};
use integer_encoding::VarInt;

use crate::{
    ed::Decode,
    element::{costs::ElementCostExtensions, tree_type::ElementTreeTypeExtensions},
    merk::NodeType,
    tree::{kv::KV, TreeNodeInner},
    tree_type::{CostSize, SUM_ITEM_COST_SIZE},
    CryptoHash, Error, Merk,
};

pub trait ElementFetchFromStorageExtensions {
    /// Get an element from Merk under a key; path should be resolved and proper
    /// Merk should be loaded by this moment
    fn get<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error>;

    /// Get an element from Merk under a key; path should be resolved and proper
    /// Merk should be loaded by this moment
    fn get_optional<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error>;

    /// Get an element directly from storage under a key
    /// Merk does not need to be loaded
    /// Errors if element doesn't exist
    fn get_from_storage<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        storage: &S,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error>;

    /// Get an element directly from storage under a key
    /// Merk does not need to be loaded
    fn get_optional_from_storage<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        storage: &S,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error>;

    /// Get an element from Merk under a key; path should be resolved and proper
    /// Merk should be loaded by this moment
    fn get_with_absolute_refs<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        path: &[&[u8]],
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error>;

    /// Get an element from Merk under a key; path should be resolved and proper
    /// Merk should be loaded by this moment
    fn get_optional_with_absolute_refs<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        path: &[&[u8]],
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error>;

    /// Get an element's value hash from Merk under a key
    fn get_value_hash<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<CryptoHash>, Error>;

    /// Get an element and its value hash from Merk under a key
    fn get_with_value_hash<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<(Element, CryptoHash), Error>;
}

trait ElementFetchFromStoragePrivateExtensions {
    /// Get an element directly from storage under a key
    /// Merk does not need to be loaded
    fn get_optional_from_storage_v0<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        storage: &S,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error>;

    /// Get an element directly from storage under a key
    /// Merk does not need to be loaded
    fn get_optional_from_storage_v1<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        storage: &S,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error>;
}

impl ElementFetchFromStorageExtensions for Element {
    /// Get an element from Merk under a key; path should be resolved and proper
    /// Merk should be loaded by this moment
    fn get<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        check_grovedb_v0_with_cost!("get", grove_version.grovedb_versions.element.get);
        Self::get_optional(merk, key.as_ref(), allow_cache, grove_version).map(|result| {
            let value = result?;
            value.ok_or_else(|| {
                let key_single_byte = if key.as_ref().len() == 1 {
                    format!("({} in decimal) ", key.as_ref().first().unwrap())
                } else {
                    String::new()
                };
                Error::PathKeyNotFound(format!(
                    "get: key 0x{} {}not found in Merk that has a root key [{}] and is of type {}",
                    hex::encode(key),
                    key_single_byte,
                    merk.root_key()
                        .map(hex::encode)
                        .unwrap_or("None".to_string()),
                    merk.merk_type,
                ))
            })
        })
    }

    /// Get an element from Merk under a key; path should be resolved and proper
    /// Merk should be loaded by this moment
    fn get_optional<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error> {
        check_grovedb_v0_with_cost!(
            "get_optional",
            grove_version.grovedb_versions.element.get_optional
        );
        let mut cost = OperationCost::default();

        let value_opt = cost_return_on_error!(
            &mut cost,
            merk.get(
                key.as_ref(),
                allow_cache,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version
            )
            .map_err(|e| Error::CorruptedData(e.to_string()))
        );
        let element = cost_return_on_error_no_add!(
            cost,
            value_opt
                .map(|value| {
                    Self::deserialize(value.as_slice(), grove_version).map_err(|_| {
                        Error::CorruptedData(String::from("unable to deserialize element"))
                    })
                })
                .transpose()
        );

        Ok(element).wrap_with_cost(cost)
    }

    /// Get an element directly from storage under a key
    /// Merk does not need to be loaded
    /// Errors if element doesn't exist
    fn get_from_storage<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        storage: &S,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        check_grovedb_v0_with_cost!(
            "get_from_storage",
            grove_version.grovedb_versions.element.get_from_storage
        );
        Self::get_optional_from_storage(storage, key.as_ref(), grove_version).map(|result| {
            let value = result?;
            value.ok_or_else(|| {
                Error::PathKeyNotFound(format!(
                    "key not found in Merk for get from storage: {}",
                    hex::encode(key)
                ))
            })
        })
    }

    /// Get an element directly from storage under a key
    /// Merk does not need to be loaded
    fn get_optional_from_storage<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        storage: &S,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error> {
        match grove_version
            .grovedb_versions
            .element
            .get_optional_from_storage
        {
            0 => Self::get_optional_from_storage_v0(storage, key, grove_version),
            1 => Self::get_optional_from_storage_v1(storage, key, grove_version),
            version => Err(Error::VersionError(
                GroveVersionError::UnknownVersionMismatch {
                    method: "get_optional_from_storage".to_string(),
                    known_versions: vec![0, 1],
                    received: version,
                },
            ))
            .wrap_with_cost(OperationCost::default()),
        }
    }

    /// Get an element from Merk under a key; path should be resolved and proper
    /// Merk should be loaded by this moment
    fn get_with_absolute_refs<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        path: &[&[u8]],
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        use crate::error::MerkErrorExt;

        check_grovedb_v0_with_cost!(
            "get_with_absolute_refs",
            grove_version
                .grovedb_versions
                .element
                .get_with_absolute_refs
        );
        let mut cost = OperationCost::default();

        let element = cost_return_on_error!(
            &mut cost,
            Self::get(merk, key.as_ref(), allow_cache, grove_version)
                .add_context(format!("path is {}", path_as_slices_hex_to_ascii(path)))
        );

        let absolute_element = cost_return_on_error_into_no_add!(
            cost,
            element.convert_if_reference_to_absolute_reference(path, Some(key.as_ref()))
        );

        Ok(absolute_element).wrap_with_cost(cost)
    }

    /// Get an element from Merk under a key; path should be resolved and proper
    /// Merk should be loaded by this moment
    fn get_optional_with_absolute_refs<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        path: &[&[u8]],
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error> {
        use crate::error::MerkErrorExt;

        check_grovedb_v0_with_cost!(
            "get_with_absolute_refs",
            grove_version
                .grovedb_versions
                .element
                .get_with_absolute_refs
        );
        let mut cost = OperationCost::default();

        let maybe_element = cost_return_on_error!(
            &mut cost,
            Self::get_optional(merk, key.as_ref(), allow_cache, grove_version)
                .add_context(format!("path is {}", path_as_slices_hex_to_ascii(path)))
        );

        match maybe_element {
            None => Ok(None).wrap_with_cost(cost),
            Some(element) => {
                let absolute_element = cost_return_on_error_into_no_add!(
                    cost,
                    element.convert_if_reference_to_absolute_reference(path, Some(key.as_ref()))
                );
                Ok(Some(absolute_element)).wrap_with_cost(cost)
            }
        }
    }

    /// Get an element's value hash from Merk under a key
    fn get_value_hash<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<CryptoHash>, Error> {
        check_grovedb_v0_with_cost!(
            "get_value_hash",
            grove_version.grovedb_versions.element.get_value_hash
        );
        let mut cost = OperationCost::default();

        let value_hash = cost_return_on_error!(
            &mut cost,
            merk.get_value_hash(
                key.as_ref(),
                allow_cache,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version
            )
            .map_err(|e| Error::CorruptedData(e.to_string()))
        );

        Ok(value_hash).wrap_with_cost(cost)
    }

    /// Get an element and its value hash from Merk under a key
    fn get_with_value_hash<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        merk: &Merk<S>,
        key: K,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<(Element, CryptoHash), Error> {
        check_grovedb_v0_with_cost!(
            "get_with_value_hash",
            grove_version.grovedb_versions.element.get_with_value_hash
        );
        let mut cost = OperationCost::default();

        let Some((value, value_hash)) = cost_return_on_error!(
            &mut cost,
            merk.get_value_and_value_hash(
                key.as_ref(),
                allow_cache,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version
            )
            .map_err(|e| Error::CorruptedData(e.to_string()))
        ) else {
            return Err(Error::PathKeyNotFound(format!(
                "get: key \"{}\" not found in Merk that has a root key [{}] and is of type {}",
                hex::encode(key),
                merk.root_key()
                    .map(hex::encode)
                    .unwrap_or("None".to_string()),
                merk.merk_type
            )))
            .wrap_with_cost(cost);
        };

        Self::deserialize(value.as_slice(), grove_version)
            .map_err(|_| Error::CorruptedData(String::from("unable to deserialize element")))
            .map(|e| (e, value_hash))
            .wrap_with_cost(cost)
    }
}

impl ElementFetchFromStoragePrivateExtensions for Element {
    /// Get an element directly from storage under a key
    /// Merk does not need to be loaded
    fn get_optional_from_storage_v0<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        storage: &S,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error> {
        let mut cost = OperationCost::default();
        let key_ref = key.as_ref();
        let node_value_opt = cost_return_on_error!(
            &mut cost,
            storage
                .get(key_ref)
                .map_err(|e| Error::CorruptedData(e.to_string()))
        );
        let maybe_tree_inner: Option<TreeNodeInner> = cost_return_on_error_no_add!(
            cost,
            node_value_opt
                .map(|node_value| {
                    Decode::decode(node_value.as_slice())
                        .map_err(|e| Error::CorruptedData(e.to_string()))
                })
                .transpose()
        );

        let value = maybe_tree_inner.map(|tree_inner| tree_inner.value_as_owned());
        let element = cost_return_on_error_no_add!(
            cost,
            value
                .as_ref()
                .map(|value| {
                    Self::deserialize(value.as_slice(), grove_version).map_err(|_| {
                        Error::CorruptedData(String::from("unable to deserialize element"))
                    })
                })
                .transpose()
        );
        match &element {
            Some(Element::Item(..)) | Some(Element::Reference(..)) => {
                // while the loaded item might be a sum item, it is given for free
                // as it would be very hard to know in advance
                cost.storage_loaded_bytes = KV::value_byte_cost_size_for_key_and_value_lengths(
                    key_ref.len() as u32,
                    value.as_ref().unwrap().len() as u32,
                    NodeType::NormalNode,
                ) as u64
            }
            Some(Element::SumItem(_, flags)) => {
                let cost_size = SUM_ITEM_COST_SIZE;
                let flags_len = flags.as_ref().map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = cost_size + flags_len;
                cost.storage_loaded_bytes = KV::node_value_byte_cost_size(
                    key_ref.len() as u32,
                    value_len,
                    NodeType::NormalNode,
                ) as u64
            }
            Some(Element::ItemWithSumItem(..)) => {
                // This should not be possible because v0 wouldn't have ItemWithSumItem
                let cost_size = SUM_ITEM_COST_SIZE;
                cost.storage_loaded_bytes = KV::value_byte_cost_size_for_key_and_value_lengths(
                    key_ref.len() as u32,
                    value.as_ref().unwrap().len() as u32 + cost_size,
                    NodeType::NormalNode,
                ) as u64
            }
            Some(Element::Tree(_, flags))
            | Some(Element::SumTree(_, _, flags))
            | Some(Element::BigSumTree(_, _, flags))
            | Some(Element::CountTree(_, _, flags))
            | Some(Element::CountSumTree(.., flags))
            | Some(Element::ProvableCountTree(_, _, flags))
            | Some(Element::ProvableCountSumTree(.., flags)) => {
                let tree_cost_size = element.as_ref().unwrap().tree_type().unwrap().cost_size();
                let flags_len = flags.as_ref().map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = tree_cost_size + flags_len;
                cost.storage_loaded_bytes =
                    KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                        key_ref.len() as u32,
                        value_len,
                        NodeType::NormalNode,
                    ) as u64
            }
            None => {}
        }
        Ok(element).wrap_with_cost(cost)
    }

    /// Get an element directly from storage under a key
    /// Merk does not need to be loaded
    fn get_optional_from_storage_v1<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        storage: &S,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error> {
        let mut cost = OperationCost::default();
        let key_ref = key.as_ref();
        let node_value_opt = cost_return_on_error!(
            &mut cost,
            storage
                .get(key_ref)
                .map_err(|e| Error::CorruptedData(e.to_string()))
        );
        let maybe_tree_inner: Option<TreeNodeInner> = cost_return_on_error_no_add!(
            cost,
            node_value_opt
                .map(|node_value| {
                    Decode::decode(node_value.as_slice())
                        .map_err(|e| Error::CorruptedData(e.to_string()))
                })
                .transpose()
        );

        let Some((value, tree_feature_type)) =
            maybe_tree_inner.map(|tree_inner| tree_inner.value_as_owned_with_feature())
        else {
            return Ok(None).wrap_with_cost(cost);
        };
        let node_type = tree_feature_type.node_type();
        let element = cost_return_on_error_no_add!(
            cost,
            Self::deserialize(value.as_slice(), grove_version).map_err(|_| {
                Error::CorruptedData(String::from("unable to deserialize element"))
            })
        );
        match &element {
            Element::Item(..) | Element::Reference(..) => {
                // while the loaded item might be a sum item, it is given for free
                // as it would be very hard to know in advance
                cost.storage_loaded_bytes = KV::value_byte_cost_size_for_key_and_value_lengths(
                    key_ref.len() as u32,
                    value.len() as u32,
                    node_type,
                ) as u64
            }
            Element::SumItem(_, flags) => {
                let cost_size = SUM_ITEM_COST_SIZE;
                let flags_len = flags.as_ref().map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = cost_size + flags_len;
                cost.storage_loaded_bytes =
                    KV::node_value_byte_cost_size(key_ref.len() as u32, value_len, node_type) as u64
                // this is changed to sum node in v1
            }
            Element::ItemWithSumItem(item_value, _, flags) => {
                let item_value_len = item_value.len() as u32;

                let cost_size = SUM_ITEM_COST_SIZE;
                let flags_len = flags.as_ref().map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len =
                    item_value_len + item_value_len.required_space() as u32 + cost_size + flags_len;
                cost.storage_loaded_bytes =
                    KV::node_value_byte_cost_size(key_ref.len() as u32, value_len, node_type) as u64
            }
            Element::Tree(_, flags)
            | Element::SumTree(_, _, flags)
            | Element::BigSumTree(_, _, flags)
            | Element::CountTree(_, _, flags)
            | Element::CountSumTree(.., flags)
            | Element::ProvableCountTree(_, _, flags)
            | Element::ProvableCountSumTree(.., flags) => {
                let tree_cost_size = element.tree_type().unwrap().cost_size();
                let flags_len = flags.as_ref().map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = tree_cost_size + flags_len;
                cost.storage_loaded_bytes =
                    KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                        key_ref.len() as u32,
                        value_len,
                        node_type,
                    ) as u64
            }
        }
        Ok(Some(element)).wrap_with_cost(cost)
    }
}

#[cfg(test)]
mod tests {
    use grovedb_path::SubtreePath;
    use grovedb_storage::{rocksdb_storage::test_utils::TempStorage, Storage, StorageBatch};

    use super::*;
    use crate::{element::insert::ElementInsertToStorageExtensions, tree_type::TreeType};

    #[test]
    fn test_cache_changes_cost() {
        let grove_version = GroveVersion::latest();
        let storage = TempStorage::new();
        let batch = StorageBatch::new();
        let transaction = storage.start_transaction();

        let ctx = storage
            .get_transactional_storage_context(SubtreePath::empty(), Some(&batch), &transaction)
            .unwrap();
        let mut merk = Merk::open_base(
            ctx,
            TreeType::NormalTree,
            Some(&Element::value_defined_cost_for_serialized_value),
            grove_version,
        )
        .unwrap()
        .unwrap();
        Element::empty_tree()
            .insert(&mut merk, b"mykey", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");
        Element::new_item(b"value".to_vec())
            .insert(&mut merk, b"another-key", None, grove_version)
            .unwrap()
            .expect("expected successful insertion 2");

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .unwrap();

        let ctx = storage
            .get_transactional_storage_context(SubtreePath::empty(), None, &transaction)
            .unwrap();
        let mut merk = Merk::open_base(
            ctx,
            TreeType::NormalTree,
            Some(&Element::value_defined_cost_for_serialized_value),
            grove_version,
        )
        .unwrap()
        .unwrap();

        assert_eq!(
            Element::get(&merk, b"another-key", true, grove_version)
                .unwrap()
                .expect("expected successful get"),
            Element::new_item(b"value".to_vec()),
        );

        // Warm up cache because the Merk was reopened.
        Element::new_item(b"value".to_vec())
            .insert(&mut merk, b"another-key", None, grove_version)
            .unwrap()
            .expect("expected successful insertion 2");

        let cost_with_cache = Element::get(&merk, b"another-key", true, grove_version)
            .cost_as_result()
            .expect("expected to get cost");
        let cost_without_cache = Element::get(&merk, b"another-key", false, grove_version)
            .cost_as_result()
            .expect("expected to get cost");
        assert_ne!(cost_with_cache, cost_without_cache);

        assert_eq!(
            cost_with_cache,
            OperationCost {
                seek_count: 0,
                storage_cost: Default::default(),
                storage_loaded_bytes: 0,
                hash_node_calls: 0,
            }
        );

        assert_eq!(
            cost_without_cache,
            OperationCost {
                seek_count: 1,
                storage_cost: Default::default(),
                storage_loaded_bytes: 75,
                hash_node_calls: 0,
            }
        );
    }
}
