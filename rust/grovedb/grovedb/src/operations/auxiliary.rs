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

//! Auxiliary operations

use grovedb_costs::{
    cost_return_on_error, storage_cost::key_value_cost::KeyValueStorageCost, CostResult, CostsExt,
    OperationCost,
};
use grovedb_path::SubtreePath;
use grovedb_storage::{Storage, StorageContext};
use grovedb_version::version::GroveVersion;

use crate::{
    element::elements_iterator::ElementIteratorExtensions, util::TxRef, Element, Error, GroveDb,
    TransactionArg,
};

impl GroveDb {
    /// Put op for aux storage
    pub fn put_aux<K: AsRef<[u8]>>(
        &self,
        key: K,
        value: &[u8],
        cost_info: Option<KeyValueStorageCost>,
        transaction: TransactionArg,
    ) -> CostResult<(), Error> {
        let mut cost = OperationCost::default();
        let tx = TxRef::new(&self.db, transaction);
        let batch = Default::default();

        let aux_storage = self
            .db
            .get_transactional_storage_context(SubtreePath::empty(), Some(&batch), tx.as_ref())
            .unwrap_add_cost(&mut cost);

        cost_return_on_error!(
            &mut cost,
            aux_storage
                .put_aux(key.as_ref(), value, cost_info)
                .map_err(Into::into)
        );

        cost_return_on_error!(
            &mut cost,
            self.db
                .commit_multi_context_batch(batch, Some(tx.as_ref()))
                .map_err(Into::into)
        );

        tx.commit_local().wrap_with_cost(cost)
    }

    /// Delete op for aux storage
    pub fn delete_aux<K: AsRef<[u8]>>(
        &self,
        key: K,
        cost_info: Option<KeyValueStorageCost>,
        transaction: TransactionArg,
    ) -> CostResult<(), Error> {
        let mut cost = OperationCost::default();
        let tx = TxRef::new(&self.db, transaction);
        let batch = Default::default();

        let aux_storage = self
            .db
            .get_transactional_storage_context(SubtreePath::empty(), Some(&batch), tx.as_ref())
            .unwrap_add_cost(&mut cost);

        cost_return_on_error!(
            &mut cost,
            aux_storage
                .delete_aux(key.as_ref(), cost_info)
                .map_err(|e| e.into())
        );

        cost_return_on_error!(
            &mut cost,
            self.db
                .commit_multi_context_batch(batch, Some(tx.as_ref()))
                .map_err(Into::into)
        );

        tx.commit_local().wrap_with_cost(cost)
    }

    /// Get op for aux storage
    pub fn get_aux<K: AsRef<[u8]>>(
        &self,
        key: K,
        transaction: TransactionArg,
    ) -> CostResult<Option<Vec<u8>>, Error> {
        let mut cost = OperationCost::default();
        let tx = TxRef::new(&self.db, transaction);

        let aux_storage = self
            .db
            .get_transactional_storage_context(SubtreePath::empty(), None, tx.as_ref())
            .unwrap_add_cost(&mut cost);

        aux_storage
            .get_aux(key.as_ref())
            .map_err(|e| e.into())
            .add_cost(cost)
    }

    // TODO: dumb traversal should not be tolerated
    /// Finds keys which are trees for a given subtree recursively.
    /// One element means a key of a `merk`, n > 1 elements mean relative path
    /// for a deeply nested subtree.
    pub fn find_subtrees<B: AsRef<[u8]>>(
        &self,
        path: &SubtreePath<B>,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<Vec<Vec<u8>>>, Error> {
        let mut cost = OperationCost::default();

        // TODO: remove conversion to vec;
        // However, it's not easy for a reason:
        // new keys to enqueue are taken from raw iterator which returns Vec<u8>;
        // changing that to slice is hard as cursor should be moved for next iteration
        // which requires exclusive (&mut) reference, also there is no guarantee that
        // slice which points into storage internals will remain valid if raw
        // iterator got altered so why that reference should be exclusive;
        //
        // Update: there are pinned views into RocksDB to return slices of data, perhaps
        // there is something for iterators

        let mut queue: Vec<Vec<Vec<u8>>> = vec![path.to_vec()];
        let mut result: Vec<Vec<Vec<u8>>> = queue.clone();

        let tx = TxRef::new(&self.db, transaction);

        while let Some(q) = queue.pop() {
            let subtree_path: SubtreePath<Vec<u8>> = q.as_slice().into();
            // Get the correct subtree with q_ref as path
            let storage = self
                .db
                .get_transactional_storage_context(subtree_path, None, tx.as_ref())
                .unwrap_add_cost(&mut cost);
            let mut raw_iter = Element::iterator(storage.raw_iter()).unwrap_add_cost(&mut cost);
            while let Some((key, value)) =
                cost_return_on_error!(&mut cost, raw_iter.next_element(grove_version))
            {
                if value.is_any_tree() {
                    let mut sub_path = q.clone();
                    sub_path.push(key.to_vec());
                    queue.push(sub_path.clone());
                    result.push(sub_path);
                }
            }
        }
        Ok(result).wrap_with_cost(cost)
    }
}
