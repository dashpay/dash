//! Prefixed storage batch implementation for RocksDB backend.

use grovedb_costs::{
    storage_cost::key_value_cost::KeyValueStorageCost, ChildrenSizesWithIsSumTree, OperationCost,
};
use integer_encoding::VarInt;
use rocksdb::{ColumnFamily, WriteBatchWithTransaction};

use super::make_prefixed_key;
use crate::{rocksdb_storage::storage::SubtreePrefix, Batch, StorageBatch};

/// Wrapper to RocksDB batch.
/// All calls go to RocksDB batch, but wrapper handles prefixes and column
/// families. Also accumulates costs before commit.
pub struct PrefixedRocksDbBatch<'db> {
    pub(crate) prefix: SubtreePrefix,
    pub(crate) batch: WriteBatchWithTransaction<true>,
    pub(crate) cf_aux: &'db ColumnFamily,
    pub(crate) cf_roots: &'db ColumnFamily,

    /// As a batch to be commited is a RocksDB batch and there is no way to get
    /// what it will do, we collect costs at the moment we append something to
    /// the batch.
    pub(crate) cost_acc: OperationCost,
}

/// Batch with no backing storage_cost (it's not a RocksDB batch, but our own
/// way to represent a set of operations) that eventually will be merged into
/// multi-context batch.
pub struct PrefixedMultiContextBatchPart {
    pub(crate) prefix: SubtreePrefix,
    pub(crate) batch: StorageBatch,
}

/// Implementation of a batch outside a transaction
impl Batch for PrefixedRocksDbBatch<'_> {
    fn put<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        children_sizes: ChildrenSizesWithIsSumTree,
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error> {
        let prefixed_key = make_prefixed_key(&self.prefix, key);

        // Update the key_storage_cost based on the prefixed key
        let updated_cost_info = cost_info.map(|mut key_value_storage_cost| {
            if key_value_storage_cost.new_node {
                // key is new, storage_cost needs to be created for it
                key_value_storage_cost.key_storage_cost.added_bytes +=
                    (prefixed_key.len() + prefixed_key.len().required_space()) as u32;
            }
            key_value_storage_cost
        });

        self.cost_acc.seek_count += 1;
        self.cost_acc.add_key_value_storage_costs(
            prefixed_key.len() as u32,
            value.len() as u32,
            children_sizes,
            updated_cost_info,
        )?;

        self.batch.put(prefixed_key, value);
        Ok(())
    }

    fn put_aux<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error> {
        let prefixed_key = make_prefixed_key(&self.prefix, key);

        self.cost_acc.seek_count += 1;
        self.cost_acc.add_key_value_storage_costs(
            prefixed_key.len() as u32,
            value.len() as u32,
            None,
            cost_info,
        )?;

        self.batch.put_cf(self.cf_aux, prefixed_key, value);
        Ok(())
    }

    fn put_root<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error> {
        let prefixed_key = make_prefixed_key(&self.prefix, key);

        self.cost_acc.seek_count += 1;
        // put root only pays if cost info is set
        if cost_info.is_some() {
            self.cost_acc.add_key_value_storage_costs(
                prefixed_key.len() as u32,
                value.len() as u32,
                None,
                cost_info,
            )?;
        }

        self.batch.put_cf(self.cf_roots, prefixed_key, value);
        Ok(())
    }

    fn delete<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>) {
        let prefixed_key = make_prefixed_key(&self.prefix, key);

        self.cost_acc.seek_count += 1;

        if let Some(removed_bytes) = cost_info {
            self.cost_acc.storage_cost.removed_bytes += removed_bytes.combined_removed_bytes();
        }

        self.batch.delete(prefixed_key);
    }

    fn delete_aux<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>) {
        let prefixed_key = make_prefixed_key(&self.prefix, key);

        self.cost_acc.seek_count += 1;

        if let Some(removed_bytes) = cost_info {
            self.cost_acc.storage_cost.removed_bytes += removed_bytes.combined_removed_bytes();
        }

        self.batch.delete_cf(self.cf_aux, prefixed_key);
    }

    fn delete_root<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>) {
        let prefixed_key = make_prefixed_key(&self.prefix, key);

        self.cost_acc.seek_count += 1;

        if let Some(removed_bytes) = cost_info {
            self.cost_acc.storage_cost.removed_bytes += removed_bytes.combined_removed_bytes();
        }

        self.batch.delete_cf(self.cf_roots, prefixed_key);
    }
}

/// Implementation of a rocksdb batch outside a transaction for multi-context
/// batch.
impl Batch for PrefixedMultiContextBatchPart {
    fn put<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        children_sizes: ChildrenSizesWithIsSumTree,
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error> {
        let prefixed_key = make_prefixed_key(&self.prefix, key);

        // Update the key_storage_cost based on the prefixed key
        let updated_cost_info = cost_info.map(|mut key_value_storage_cost| {
            if key_value_storage_cost.new_node {
                // key is new, storage_cost needs to be created for it
                key_value_storage_cost.key_storage_cost.added_bytes +=
                    (prefixed_key.len() + prefixed_key.len().required_space()) as u32;
            }
            key_value_storage_cost
        });

        self.batch.put(
            prefixed_key,
            value.to_vec(),
            children_sizes,
            updated_cost_info,
        );
        Ok(())
    }

    fn put_aux<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error> {
        self.batch.put_aux(
            make_prefixed_key(&self.prefix, key),
            value.to_vec(),
            cost_info,
        );
        Ok(())
    }

    fn put_root<K: AsRef<[u8]>>(
        &mut self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), grovedb_costs::error::Error> {
        self.batch.put_root(
            make_prefixed_key(&self.prefix, key),
            value.to_vec(),
            cost_info,
        );
        Ok(())
    }

    fn delete<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>) {
        self.batch
            .delete(make_prefixed_key(&self.prefix, key), cost_info);
    }

    fn delete_aux<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>) {
        self.batch
            .delete_aux(make_prefixed_key(&self.prefix, key), cost_info);
    }

    fn delete_root<K: AsRef<[u8]>>(&mut self, key: K, cost_info: Option<KeyValueStorageCost>) {
        self.batch
            .delete_root(make_prefixed_key(&self.prefix, key), cost_info);
    }
}
