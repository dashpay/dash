// MIT LICENSE
//
// Copyright (c) 2021 Dash Core Group
//
// Permission is hereby granted, free of charge, to any
// person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the
// Software without restriction, including without
// limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice
// shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

//! Storage for GroveDB

use std::{
    cell::RefCell,
    collections::{btree_map::IntoValues, BTreeMap},
    path::Path,
};

use grovedb_costs::{
    storage_cost::key_value_cost::KeyValueStorageCost, ChildrenSizesWithIsSumTree, CostContext,
    CostResult, OperationCost,
};
use grovedb_path::SubtreePath;
use grovedb_visualize::visualize_to_vec;

use crate::{worst_case_costs::WorstKeyLength, Error};
pub type SubtreePrefix = [u8; 32];

/// Top-level storage_cost abstraction.
/// Should be able to hold storage_cost connection and to start transaction when
/// needed. All query operations will be exposed using [StorageContext].
pub trait Storage<'db> {
    /// Storage transaction type
    type Transaction;

    /// Storage context type for multi-tree batch operations inside transaction
    type BatchTransactionalStorageContext: StorageContext<'db>;

    /// Storage context type for direct writes to the storage. The only use case
    /// is replication process.
    type ImmediateStorageContext: StorageContext<'db>;

    /// Starts a new transaction
    fn start_transaction(&'db self) -> Self::Transaction;

    /// Consumes and commits a transaction
    fn commit_transaction(&self, transaction: Self::Transaction) -> CostResult<(), Error>;

    /// Rollback a transaction
    fn rollback_transaction(&self, transaction: &Self::Transaction) -> Result<(), Error>;

    /// Consumes and applies multi-context batch.
    fn commit_multi_context_batch(
        &self,
        batch: StorageBatch,
        transaction: Option<&'db Self::Transaction>,
    ) -> CostResult<(), Error>;

    /// Forces data to be written
    fn flush(&self) -> Result<(), Error>;

    /// Make context for a subtree on transactional data, keeping all write
    /// operations inside a `batch` if provided.
    fn get_transactional_storage_context<'b, B>(
        &'db self,
        path: SubtreePath<'b, B>,
        batch: Option<&'db StorageBatch>,
        transaction: &'db Self::Transaction,
    ) -> CostContext<Self::BatchTransactionalStorageContext>
    where
        B: AsRef<[u8]> + 'b;

    /// Make context for a subtree by prefix on transactional data, keeping all
    /// write operations inside a `batch` if provided.
    fn get_transactional_storage_context_by_subtree_prefix(
        &'db self,
        prefix: SubtreePrefix,
        batch: Option<&'db StorageBatch>,
        transaction: &'db Self::Transaction,
    ) -> CostContext<Self::BatchTransactionalStorageContext>;

    /// Make context for a subtree on transactional data that will apply all
    /// operations straight to the storage.
    fn get_immediate_storage_context<'b, B>(
        &'db self,
        path: SubtreePath<'b, B>,
        transaction: &'db Self::Transaction,
    ) -> CostContext<Self::ImmediateStorageContext>
    where
        B: AsRef<[u8]> + 'b;

    /// Make context for a subtree by prefix on transactional data that will
    /// apply all operations straight to the storage.
    fn get_immediate_storage_context_by_subtree_prefix(
        &'db self,
        prefix: SubtreePrefix,
        transaction: &'db Self::Transaction,
    ) -> CostContext<Self::ImmediateStorageContext>;

    /// Creates a database checkpoint in a specified path
    fn create_checkpoint<P: AsRef<Path>>(&self, path: P) -> Result<(), Error>;

    /// Return worst case cost for storage_cost context creation.
    fn get_storage_context_cost<L: WorstKeyLength>(path: &[L]) -> OperationCost;
}

pub use grovedb_costs::ChildrenSizes;

/// Storage context.
/// Provides operations expected from a database abstracting details such as
/// whether it is a transaction or not.
pub trait StorageContext<'db> {
    /// Storage batch type
    type Batch: Batch;

    /// Storage raw iterator type (to iterate over storage_cost without
    /// supplying a key)
    type RawIterator: RawIterator;

    /// Put `value` into data storage_cost with `key`
    fn put<K: AsRef<[u8]>>(
        &self,
        key: K,
        value: &[u8],
        children_sizes: ChildrenSizesWithIsSumTree,
        cost_info: Option<KeyValueStorageCost>,
    ) -> CostResult<(), Error>;

    /// Put `value` into auxiliary data storage_cost with `key`
    fn put_aux<K: AsRef<[u8]>>(
        &self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> CostResult<(), Error>;

    /// Put `value` into trees roots storage_cost with `key`
    fn put_root<K: AsRef<[u8]>>(
        &self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> CostResult<(), Error>;

    /// Put `value` into GroveDB metadata storage_cost with `key`
    fn put_meta<K: AsRef<[u8]>>(
        &self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> CostResult<(), Error>;

    /// Delete entry with `key` from data storage_cost
    fn delete<K: AsRef<[u8]>>(
        &self,
        key: K,
        cost_info: Option<KeyValueStorageCost>,
    ) -> CostResult<(), Error>;

    /// Delete entry with `key` from auxiliary data storage_cost
    fn delete_aux<K: AsRef<[u8]>>(
        &self,
        key: K,
        cost_info: Option<KeyValueStorageCost>,
    ) -> CostResult<(), Error>;

    /// Delete entry with `key` from trees roots storage_cost
    fn delete_root<K: AsRef<[u8]>>(
        &self,
        key: K,
        cost_info: Option<KeyValueStorageCost>,
    ) -> CostResult<(), Error>;

    /// Delete entry with `key` from GroveDB metadata storage_cost
    fn delete_meta<K: AsRef<[u8]>>(
        &self,
        key: K,
        cost_info: Option<KeyValueStorageCost>,
    ) -> CostResult<(), Error>;

    /// Get entry by `key` from data storage_cost
    fn get<K: AsRef<[u8]>>(&self, key: K) -> CostResult<Option<Vec<u8>>, Error>;

    /// Get entry by `key` from auxiliary data storage_cost
    fn get_aux<K: AsRef<[u8]>>(&self, key: K) -> CostResult<Option<Vec<u8>>, Error>;

    /// Get entry by `key` from trees roots storage_cost
    fn get_root<K: AsRef<[u8]>>(&self, key: K) -> CostResult<Option<Vec<u8>>, Error>;

    /// Get entry by `key` from GroveDB metadata storage_cost
    fn get_meta<K: AsRef<[u8]>>(&self, key: K) -> CostResult<Option<Vec<u8>>, Error>;

    /// Initialize a new batch
    fn new_batch(&self) -> Self::Batch;

    /// Commits changes from batch into storage
    fn commit_batch(&self, batch: Self::Batch) -> CostResult<(), Error>;

    /// Get raw iterator over storage_cost
    fn raw_iter(&self) -> Self::RawIterator;
}

/// Database batch (not to be confused with multi-tree operations batch).
pub trait Batch {
    /// Appends to the database batch a put operation for a data record.
    fn put<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        children_sizes: ChildrenSizesWithIsSumTree,
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error>;

    /// Appends to the database batch a put operation for aux storage_cost.
    fn put_aux<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error>;

    /// Appends to the database batch a put operation for subtrees roots
    /// storage_cost.
    fn put_root<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error>;

    /// Appends to the database batch a delete operation for a data record.
    fn delete<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>);

    /// Appends to the database batch a delete operation for aux storage_cost.
    fn delete_aux<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>);

    /// Appends to the database batch a delete operation for a record in subtree
    /// roots storage_cost.
    fn delete_root<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>);
}

/// Allows to iterate over database record inside of storage_cost context.
pub trait RawIterator {
    /// Move iterator to first valid record.
    fn seek_to_first(&mut self) -> CostContext<()>;

    /// Move iterator to last valid record.
    fn seek_to_last(&mut self) -> CostContext<()>;

    /// Move iterator forward until `key` is hit.
    fn seek<K: AsRef<[u8]>>(&mut self, key: K) -> CostContext<()>;

    /// Move iterator backward until `key` is hit.
    fn seek_for_prev<K: AsRef<[u8]>>(&mut self, key: K) -> CostContext<()>;

    /// Move iterator to next record.
    fn next(&mut self) -> CostContext<()>;

    /// Move iterator to previous record.
    fn prev(&mut self) -> CostContext<()>;

    /// Return value of key-value pair where raw iterator points at.
    fn value(&self) -> CostContext<Option<&[u8]>>;

    /// Return key of key-value pair where raw iterator points at.
    fn key(&self) -> CostContext<Option<&[u8]>>;

    /// Check if raw iterator points into a valid record
    fn valid(&self) -> CostContext<bool>;
}

/// Structure to hold deferred database operations in "batched" storage_cost
/// contexts.
#[derive(Debug)]
pub struct StorageBatch {
    operations: RefCell<Operations>,
}

#[derive(Default)]
struct Operations {
    data: BTreeMap<Vec<u8>, AbstractBatchOperation>,
    roots: BTreeMap<Vec<u8>, AbstractBatchOperation>,
    aux: BTreeMap<Vec<u8>, AbstractBatchOperation>,
    meta: BTreeMap<Vec<u8>, AbstractBatchOperation>,
}

impl std::fmt::Debug for Operations {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut fmt = f.debug_struct("Operations");

        fmt.field("data", &self.data.values());
        fmt.field("aux", &self.aux.values());
        fmt.field("roots", &self.roots.values());
        fmt.field("meta", &self.meta.values());

        fmt.finish()
    }
}

impl StorageBatch {
    /// Create empty batch.
    pub fn new() -> Self {
        StorageBatch {
            operations: RefCell::new(Operations::default()),
        }
    }

    /// Get batch length
    pub fn len(&self) -> usize {
        let operations = self.operations.borrow();
        operations.data.len()
            + operations.roots.len()
            + operations.aux.len()
            + operations.meta.len()
    }

    /// Batch emptiness predicate
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Add deferred `put` operation
    pub(crate) fn put(
        &self,
        key: Vec<u8>,
        value: Vec<u8>,
        children_sizes: ChildrenSizesWithIsSumTree,
        cost_info: Option<KeyValueStorageCost>,
    ) {
        self.operations.borrow_mut().data.insert(
            key.clone(),
            AbstractBatchOperation::Put {
                key,
                value,
                children_sizes,
                cost_info,
            },
        );
    }

    /// Add deferred `put` operation for aux storage_cost
    pub(crate) fn put_aux(
        &self,
        key: Vec<u8>,
        value: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    ) {
        self.operations.borrow_mut().aux.insert(
            key.clone(),
            AbstractBatchOperation::PutAux {
                key,
                value,
                cost_info,
            },
        );
    }

    /// Add deferred `put` operation for subtree roots storage_cost
    pub(crate) fn put_root(
        &self,
        key: Vec<u8>,
        value: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    ) {
        self.operations.borrow_mut().roots.insert(
            key.clone(),
            AbstractBatchOperation::PutRoot {
                key,
                value,
                cost_info,
            },
        );
    }

    /// Add deferred `put` operation for metadata storage_cost
    pub(crate) fn put_meta(
        &self,
        key: Vec<u8>,
        value: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    ) {
        self.operations.borrow_mut().meta.insert(
            key.clone(),
            AbstractBatchOperation::PutMeta {
                key,
                value,
                cost_info,
            },
        );
    }

    /// Add deferred `delete` operation
    pub(crate) fn delete(&self, key: Vec<u8>, cost_info: Option<KeyValueStorageCost>) {
        let operations = &mut self.operations.borrow_mut().data;
        if operations.get(&key).is_none() {
            operations.insert(
                key.clone(),
                AbstractBatchOperation::Delete { key, cost_info },
            );
        }
    }

    /// Add deferred `delete` operation for aux storage_cost
    pub(crate) fn delete_aux(&self, key: Vec<u8>, cost_info: Option<KeyValueStorageCost>) {
        let operations = &mut self.operations.borrow_mut().aux;
        if operations.get(&key).is_none() {
            operations.insert(
                key.clone(),
                AbstractBatchOperation::DeleteAux { key, cost_info },
            );
        }
    }

    /// Add deferred `delete` operation for subtree roots storage_cost
    pub(crate) fn delete_root(&self, key: Vec<u8>, cost_info: Option<KeyValueStorageCost>) {
        let operations = &mut self.operations.borrow_mut().roots;
        if operations.get(&key).is_none() {
            operations.insert(
                key.clone(),
                AbstractBatchOperation::DeleteRoot { key, cost_info },
            );
        }
    }

    /// Add deferred `delete` operation for metadata storage_cost
    pub(crate) fn delete_meta(&self, key: Vec<u8>, cost_info: Option<KeyValueStorageCost>) {
        let operations = &mut self.operations.borrow_mut().meta;
        if operations.get(&key).is_none() {
            operations.insert(
                key.clone(),
                AbstractBatchOperation::DeleteMeta { key, cost_info },
            );
        }
    }

    /// Merge batch into this one
    pub(crate) fn merge(&self, other: StorageBatch) {
        for op in other.into_iter() {
            match op {
                AbstractBatchOperation::Put {
                    key,
                    value,
                    children_sizes,
                    cost_info,
                } => self.put(key, value, children_sizes, cost_info),
                AbstractBatchOperation::PutAux {
                    key,
                    value,
                    cost_info,
                } => self.put_aux(key, value, cost_info),
                AbstractBatchOperation::PutRoot {
                    key,
                    value,
                    cost_info,
                } => self.put_root(key, value, cost_info),
                AbstractBatchOperation::PutMeta {
                    key,
                    value,
                    cost_info,
                } => self.put_meta(key, value, cost_info),
                AbstractBatchOperation::Delete { key, cost_info } => self.delete(key, cost_info),
                AbstractBatchOperation::DeleteAux { key, cost_info } => {
                    self.delete_aux(key, cost_info)
                }
                AbstractBatchOperation::DeleteRoot { key, cost_info } => {
                    self.delete_root(key, cost_info)
                }
                AbstractBatchOperation::DeleteMeta { key, cost_info } => {
                    self.delete_meta(key, cost_info)
                }
            }
        }
    }
}

/// Iterator over storage_cost batch operations.
pub(crate) struct StorageBatchIter {
    data: IntoValues<Vec<u8>, AbstractBatchOperation>,
    aux: IntoValues<Vec<u8>, AbstractBatchOperation>,
    meta: IntoValues<Vec<u8>, AbstractBatchOperation>,
    roots: IntoValues<Vec<u8>, AbstractBatchOperation>,
}

impl Iterator for StorageBatchIter {
    type Item = AbstractBatchOperation;

    fn next(&mut self) -> Option<Self::Item> {
        self.meta
            .next()
            .or_else(|| self.aux.next())
            .or_else(|| self.roots.next())
            .or_else(|| self.data.next())
    }
}

// Making this a method rather than `IntoIter` implementation as we don't want
// to leak multi context batch internals in any way
impl StorageBatch {
    pub(crate) fn into_iter(self) -> StorageBatchIter {
        let operations = self.operations.into_inner();

        StorageBatchIter {
            data: operations.data.into_values(),
            aux: operations.aux.into_values(),
            meta: operations.meta.into_values(),
            roots: operations.roots.into_values(),
        }
    }
}

impl Default for StorageBatch {
    fn default() -> Self {
        Self::new()
    }
}

/// Deferred storage_cost operation not tied to any storage_cost implementation,
/// required for multi-tree batches.
#[allow(missing_docs)]
#[derive(strum::AsRefStr)]
pub(crate) enum AbstractBatchOperation {
    /// Deferred put operation
    Put {
        key: Vec<u8>,
        value: Vec<u8>,
        children_sizes: ChildrenSizesWithIsSumTree,
        cost_info: Option<KeyValueStorageCost>,
    },
    /// Deferred put operation for aux storage_cost
    PutAux {
        key: Vec<u8>,
        value: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    },
    /// Deferred put operation for roots storage_cost
    PutRoot {
        key: Vec<u8>,
        value: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    },
    /// Deferred put operation for metadata storage_cost
    PutMeta {
        key: Vec<u8>,
        value: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    },
    /// Deferred delete operation
    Delete {
        key: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    },
    /// Deferred delete operation for aux storage_cost
    DeleteAux {
        key: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    },
    /// Deferred delete operation for roots storage_cost
    DeleteRoot {
        key: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    },
    /// Deferred delete operation for metadata storage_cost
    DeleteMeta {
        key: Vec<u8>,
        cost_info: Option<KeyValueStorageCost>,
    },
}

impl std::fmt::Debug for AbstractBatchOperation {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut fmt = f.debug_struct(self.as_ref());

        let mut key_buf = Vec::new();
        let mut value_buf = Vec::new();

        match self {
            AbstractBatchOperation::Put { key, value, .. }
            | AbstractBatchOperation::PutAux { key, value, .. }
            | AbstractBatchOperation::PutMeta { key, value, .. }
            | AbstractBatchOperation::PutRoot { key, value, .. } => {
                key_buf.clear();
                value_buf.clear();
                visualize_to_vec(&mut key_buf, key.as_slice());
                visualize_to_vec(&mut value_buf, value.as_slice());
                fmt.field("key", &String::from_utf8_lossy(&key_buf))
                    .field("value", &String::from_utf8_lossy(&value_buf));
            }
            AbstractBatchOperation::Delete { key, .. }
            | AbstractBatchOperation::DeleteAux { key, .. }
            | AbstractBatchOperation::DeleteMeta { key, .. }
            | AbstractBatchOperation::DeleteRoot { key, .. } => {
                key_buf.clear();
                visualize_to_vec(&mut key_buf, key.as_slice());
                fmt.field("key", &String::from_utf8_lossy(&key_buf));
            }
        }

        fmt.finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_debug_output_batch_operation() {
        let op1 = AbstractBatchOperation::PutMeta {
            key: b"key1".to_vec(),
            value: b"value1".to_vec(),
            cost_info: None,
        };
        let op2 = AbstractBatchOperation::DeleteRoot {
            key: b"key1".to_vec(),
            cost_info: None,
        };
        assert_eq!(
            format!("{op1:?}"),
            "PutMeta { key: \"[hex: 6b657931, str: key1]\", value: \"[hex: 76616c756531, str: \
             value1]\" }"
        );
        assert_eq!(
            format!("{op2:?}"),
            "DeleteRoot { key: \"[hex: 6b657931, str: key1]\" }"
        );
    }
}
