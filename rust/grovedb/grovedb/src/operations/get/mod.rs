//! Get operations and costs

#[cfg(feature = "estimated_costs")]
mod average_case;

mod query;
use grovedb_storage::Storage;
pub use query::QueryItemOrSumReturnType;
#[cfg(feature = "estimated_costs")]
mod worst_case;

use std::collections::HashSet;

use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_into, cost_return_on_error_no_add, CostResult,
    CostsExt, OperationCost,
};
use grovedb_merk::{element::get::ElementFetchFromStorageExtensions, error::MerkErrorExt};
use grovedb_path::SubtreePath;
use grovedb_storage::StorageContext;
use grovedb_version::{check_grovedb_v0_with_cost, version::GroveVersion};

use crate::{
    reference_path::{path_from_reference_path_type, path_from_reference_qualified_path_type},
    util::TxRef,
    Element, Error, GroveDb, Transaction, TransactionArg,
};

/// Limit of possible indirections
pub const MAX_REFERENCE_HOPS: usize = 10;

impl GroveDb {
    /// Get an element from the backing store
    /// Merk Caching is on by default
    /// use get_caching_optional if no caching is desired
    pub fn get<'b, B, P>(
        &self,
        path: P,
        key: &[u8],
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error>
    where
        B: AsRef<[u8]> + 'b,
        P: Into<SubtreePath<'b, B>>,
    {
        check_grovedb_v0_with_cost!("get", grove_version.grovedb_versions.operations.get.get);

        self.get_caching_optional(path.into(), key, true, transaction, grove_version)
    }

    /// Get an element from the backing store
    /// Merk Caching can be set
    pub fn get_caching_optional<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        key: &[u8],
        allow_cache: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        check_grovedb_v0_with_cost!(
            "get_caching_optional",
            grove_version
                .grovedb_versions
                .operations
                .get
                .get_caching_optional
        );

        let mut cost = OperationCost::default();

        match cost_return_on_error!(
            &mut cost,
            self.get_raw_caching_optional(
                path.clone(),
                key,
                allow_cache,
                transaction,
                grove_version
            )
        ) {
            Element::Reference(reference_path, ..) => {
                let path_owned = cost_return_on_error_into!(
                    &mut cost,
                    path_from_reference_path_type(reference_path, &path.to_vec(), Some(key))
                        .wrap_with_cost(OperationCost::default())
                );
                self.follow_reference(
                    path_owned.as_slice().into(),
                    allow_cache,
                    transaction,
                    grove_version,
                )
                .add_cost(cost)
            }
            other => Ok(other).wrap_with_cost(cost),
        }
    }

    /// Return the Element that a reference points to.
    /// If the reference points to another reference, keep following until
    /// base element is reached.
    pub fn follow_reference<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        allow_cache: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        check_grovedb_v0_with_cost!(
            "follow_reference",
            grove_version
                .grovedb_versions
                .operations
                .get
                .follow_reference
        );

        let mut cost = OperationCost::default();

        let mut hops_left = MAX_REFERENCE_HOPS;
        let mut current_element;
        let mut visited = HashSet::new();
        // TODO, still have to do because of references handling
        let mut current_path = path.to_vec();

        while hops_left > 0 {
            if visited.contains(&current_path) {
                return Err(Error::CyclicReference).wrap_with_cost(cost);
            }
            if let Some((key, path_slice)) = current_path.split_last() {
                current_element = cost_return_on_error!(
                    &mut cost,
                    self.get_raw_caching_optional(
                        path_slice.into(),
                        key,
                        allow_cache,
                        transaction,
                        grove_version
                    )
                    .map_err(|e| match e {
                        Error::PathParentLayerNotFound(p) => {
                            Error::CorruptedReferencePathParentLayerNotFound(p)
                        }
                        Error::PathKeyNotFound(p) => {
                            Error::CorruptedReferencePathKeyNotFound(p)
                        }
                        Error::PathNotFound(p) => {
                            Error::CorruptedReferencePathNotFound(p)
                        }
                        _ => e,
                    })
                )
            } else {
                return Err(Error::CorruptedPath("empty path".to_string())).wrap_with_cost(cost);
            }
            visited.insert(current_path.clone());
            match current_element {
                Element::Reference(reference_path, ..) => {
                    current_path = cost_return_on_error_into!(
                        &mut cost,
                        path_from_reference_qualified_path_type(reference_path, &current_path)
                            .wrap_with_cost(OperationCost::default())
                    )
                }
                other => return Ok(other).wrap_with_cost(cost),
            }
            hops_left -= 1;
        }
        Err(Error::ReferenceLimit).wrap_with_cost(cost)
    }

    /// Get Element at specified path and key
    /// If element is a reference return as is, don't follow
    pub fn get_raw<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        key: &[u8],
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        check_grovedb_v0_with_cost!(
            "get_raw",
            grove_version.grovedb_versions.operations.get.get_raw
        );

        self.get_raw_caching_optional(path, key, true, transaction, grove_version)
    }

    /// Get tree item without following references
    pub fn get_raw_caching_optional<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        key: &[u8],
        allow_cache: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        check_grovedb_v0_with_cost!(
            "get_raw_caching_optional",
            grove_version
                .grovedb_versions
                .operations
                .get
                .get_raw_caching_optional
        );

        let tx = TxRef::new(&self.db, transaction);

        self.get_raw_on_transaction_caching_optional(
            path,
            key,
            allow_cache,
            tx.as_ref(),
            grove_version,
        )
    }

    /// Get Element at specified path and key
    /// If element is a reference return as is, don't follow
    /// Return None if element is not found
    pub fn get_raw_optional<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        key: &[u8],
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error> {
        check_grovedb_v0_with_cost!(
            "get_raw_optional",
            grove_version
                .grovedb_versions
                .operations
                .get
                .get_raw_optional
        );

        self.get_raw_optional_caching_optional(path, key, true, transaction, grove_version)
    }

    /// Get tree item without following references
    pub fn get_raw_optional_caching_optional<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        key: &[u8],
        allow_cache: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error> {
        check_grovedb_v0_with_cost!(
            "get_raw_optional_caching_optional",
            grove_version
                .grovedb_versions
                .operations
                .get
                .get_raw_optional_caching_optional
        );

        let tx = TxRef::new(&self.db, transaction);

        self.get_raw_optional_on_transaction_caching_optional(
            path,
            key,
            allow_cache,
            tx.as_ref(),
            grove_version,
        )
    }

    /// Get tree item without following references
    pub(crate) fn get_raw_on_transaction_caching_optional<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        key: &[u8],
        allow_cache: bool,
        transaction: &Transaction,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        let mut cost = OperationCost::default();

        let merk_to_get_from = cost_return_on_error!(
            &mut cost,
            self.open_transactional_merk_at_path(path.clone(), transaction, None, grove_version)
                .map_err(|e| match e {
                    Error::InvalidParentLayerPath(s) => {
                        Error::PathParentLayerNotFound(s)
                    }
                    _ => e,
                })
        );

        Element::get(&merk_to_get_from, key, allow_cache, grove_version)
            .add_context(format!("path is {}", path))
            .map_err(|e| e.into())
            .add_cost(cost)
    }

    /// Get tree item without following references
    pub(crate) fn get_raw_optional_on_transaction_caching_optional<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        key: &[u8],
        allow_cache: bool,
        transaction: &Transaction,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Element>, Error> {
        let mut cost = OperationCost::default();
        let merk_result = self
            .open_transactional_merk_at_path(path, transaction, None, grove_version)
            .map_err(|e| match e {
                Error::InvalidParentLayerPath(s) => Error::PathParentLayerNotFound(s),
                _ => e,
            })
            .unwrap_add_cost(&mut cost);
        let merk = cost_return_on_error_no_add!(
            cost,
            match merk_result {
                Ok(result) => Ok(Some(result)),
                Err(Error::PathParentLayerNotFound(_)) | Err(Error::InvalidParentLayerPath(_)) =>
                    Ok(None),
                Err(e) => Err(e),
            }
        );

        if let Some(merk_to_get_from) = merk {
            Element::get_optional(&merk_to_get_from, key, allow_cache, grove_version)
                .map_err(|e| e.into())
                .add_cost(cost)
        } else {
            Ok(None).wrap_with_cost(cost)
        }
    }

    /// Does tree element exist without following references
    /// There is no cache for has_raw
    pub fn has_raw<'b, B, P>(
        &self,
        path: P,
        key: &[u8],
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<bool, Error>
    where
        B: AsRef<[u8]> + 'b,
        P: Into<SubtreePath<'b, B>>,
    {
        check_grovedb_v0_with_cost!(
            "has_raw",
            grove_version.grovedb_versions.operations.get.has_raw
        );

        let tx = TxRef::new(&self.db, transaction);

        // Merk's items should be written into data storage and checked accordingly
        self.db
            .get_transactional_storage_context(path.into(), None, tx.as_ref())
            .flat_map(|s| s.get(key).map_err(|e| e.into()).map_ok(|x| x.is_some()))
    }

    fn check_subtree_exists<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        transaction: &Transaction,
        error_fn: impl FnOnce() -> Error,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        let mut cost = OperationCost::default();

        if let Some((parent_path, parent_key)) = path.derive_parent() {
            let element = {
                let merk_to_get_from = cost_return_on_error!(
                    &mut cost,
                    self.open_transactional_merk_at_path(
                        parent_path,
                        transaction,
                        None,
                        grove_version
                    )
                );

                Element::get(&merk_to_get_from, parent_key, true, grove_version)
                    .add_context(format!("path is {}", path))
                    .map_err(|e| e.into())
            }
            .unwrap_add_cost(&mut cost);
            match element {
                Ok(Element::Tree(..))
                | Ok(Element::SumTree(..))
                | Ok(Element::BigSumTree(..))
                | Ok(Element::CountTree(..))
                | Ok(Element::CountSumTree(..)) => Ok(()).wrap_with_cost(cost),
                Ok(_) | Err(Error::PathKeyNotFound(_)) => Err(error_fn()).wrap_with_cost(cost),
                Err(e) => Err(e).wrap_with_cost(cost),
            }
        } else {
            Ok(()).wrap_with_cost(cost)
        }
    }

    /// Check that subtree exists with path not found error
    pub(crate) fn check_subtree_exists_path_not_found<'b, B>(
        &self,
        path: SubtreePath<'b, B>,
        transaction: &Transaction,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error>
    where
        B: AsRef<[u8]> + 'b,
    {
        self.check_subtree_exists(
            path.clone(),
            transaction,
            || {
                Error::PathNotFound(format!(
                    "subtree doesn't exist at path {:?}",
                    path.to_vec()
                        .into_iter()
                        .map(hex::encode)
                        .collect::<Vec<String>>()
                ))
            },
            grove_version,
        )
    }

    /// Check subtree exists with invalid path error
    pub fn check_subtree_exists_invalid_path<B: AsRef<[u8]>>(
        &self,
        path: SubtreePath<B>,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        check_grovedb_v0_with_cost!(
            "check_subtree_exists_invalid_path",
            grove_version
                .grovedb_versions
                .operations
                .get
                .check_subtree_exists_invalid_path
        );

        let tx = TxRef::new(&self.db, transaction);

        self.check_subtree_exists(
            path,
            tx.as_ref(),
            || Error::InvalidPath("subtree doesn't exist".to_owned()),
            grove_version,
        )
    }
}
