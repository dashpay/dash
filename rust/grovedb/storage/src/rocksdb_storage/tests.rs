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

//! Tests

use super::test_utils::TempStorage;
use crate::Batch;

mod immediate_storage {
    use super::*;
    use crate::{RawIterator, Storage, StorageContext};

    #[test]
    fn test_aux_cf_methods() {
        let storage = TempStorage::new();
        let tx = storage.start_transaction();
        let context_ayya = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx)
            .unwrap();
        let context_ayyb = storage
            .get_immediate_storage_context([b"ayyb"].as_ref().into(), &tx)
            .unwrap();

        context_ayya
            .put_aux(b"key1", b"ayyavalue1", None)
            .unwrap()
            .expect("cannot insert into aux cf");
        context_ayya
            .put_aux(b"key2", b"ayyavalue2", None)
            .unwrap()
            .expect("cannot insert into aux cf");
        context_ayyb
            .put_aux(b"key1", b"ayybvalue1", None)
            .unwrap()
            .expect("cannot insert into aux cf");
        context_ayyb
            .put_aux(b"key2", b"ayybvalue2", None)
            .unwrap()
            .expect("cannot insert into aux cf");

        assert_eq!(
            context_ayya
                .get_aux(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from aux cf"),
            b"ayyavalue1"
        );

        storage
            .commit_transaction(tx)
            .unwrap()
            .expect("cannot commit transaction");

        let tx2 = storage.start_transaction();
        let context_ayya_after_tx = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx2)
            .unwrap();
        let tx3 = storage.start_transaction();
        let context_ayya_after_no_tx = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx3)
            .unwrap();

        context_ayya_after_tx
            .delete_aux(b"key1", None)
            .unwrap()
            .expect("cannot delete from aux cf");

        // Should be deleted inside transaction:
        assert!(context_ayya_after_tx
            .get_aux(b"key1")
            .unwrap()
            .expect("cannot get from aux cf")
            .is_none());

        // But still accessible outside of it:
        assert_eq!(
            context_ayya_after_no_tx
                .get_aux(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from aux cf"),
            b"ayyavalue1"
        );

        storage
            .commit_transaction(tx2)
            .unwrap()
            .expect("cannot commit transaction");

        // ... and no longer accessible at all after transaciton got commited
        assert!(context_ayya_after_no_tx
            .get_aux(b"key1")
            .unwrap()
            .ok()
            .expect("cannot get from aux cf")
            .is_none());
    }

    #[test]
    fn test_roots_cf_methods() {
        let storage = TempStorage::new();
        let tx = storage.start_transaction();
        let context_ayya = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx)
            .unwrap();
        let context_ayyb = storage
            .get_immediate_storage_context([b"ayyb"].as_ref().into(), &tx)
            .unwrap();

        context_ayya
            .put_root(b"key1", b"ayyavalue1", None)
            .unwrap()
            .expect("cannot insert into roots cf");
        context_ayya
            .put_root(b"key2", b"ayyavalue2", None)
            .unwrap()
            .expect("cannot insert into roots cf");
        context_ayyb
            .put_root(b"key1", b"ayybvalue1", None)
            .unwrap()
            .expect("cannot insert into roots cf");
        context_ayyb
            .put_root(b"key2", b"ayybvalue2", None)
            .unwrap()
            .expect("cannot insert into roots cf");

        assert_eq!(
            context_ayya
                .get_root(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from roots cf"),
            b"ayyavalue1"
        );

        storage
            .commit_transaction(tx)
            .unwrap()
            .expect("cannot commit transaction");

        let tx2 = storage.start_transaction();
        let context_ayya_after_tx = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx2)
            .unwrap();
        let tx3 = storage.start_transaction();
        let context_ayya_after_no_tx = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx3)
            .unwrap();

        context_ayya_after_tx
            .delete_root(b"key1", None)
            .unwrap()
            .expect("cannot delete from roots cf");

        // Should be deleted inside transaction:
        assert!(context_ayya_after_tx
            .get_root(b"key1")
            .unwrap()
            .expect("cannot get from roots cf")
            .is_none());

        // But still accessible outside of it:
        assert_eq!(
            context_ayya_after_no_tx
                .get_root(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from roots cf"),
            b"ayyavalue1"
        );

        storage
            .commit_transaction(tx2)
            .unwrap()
            .expect("cannot commit transaction");

        // ... and no longer accessible at all after transaciton got commited
        assert!(context_ayya_after_no_tx
            .get_root(b"key1")
            .unwrap()
            .ok()
            .expect("cannot get from roots cf")
            .is_none());
    }

    #[test]
    fn test_meta_cf_methods() {
        let storage = TempStorage::new();
        let tx = storage.start_transaction();
        let context_ayya = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx)
            .unwrap();
        let context_ayyb = storage
            .get_immediate_storage_context([b"ayyb"].as_ref().into(), &tx)
            .unwrap();

        context_ayya
            .put_meta(b"key1", b"ayyavalue1", None)
            .unwrap()
            .expect("cannot insert into meta cf");
        context_ayya
            .put_meta(b"key2", b"ayyavalue2", None)
            .unwrap()
            .expect("cannot insert into meta cf");
        context_ayyb
            .put_meta(b"key1", b"ayybvalue1", None)
            .unwrap()
            .expect("cannot insert into meta cf");
        context_ayyb
            .put_meta(b"key2", b"ayybvalue2", None)
            .unwrap()
            .expect("cannot insert into meta cf");

        assert_eq!(
            context_ayya
                .get_meta(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from meta cf"),
            b"ayyavalue1"
        );

        context_ayya
            .delete_meta(b"key1", None)
            .unwrap()
            .expect("cannot delete from meta cf");

        assert!(context_ayya
            .get_meta(b"key1")
            .unwrap()
            .expect("cannot get from meta cf")
            .is_none());
        assert_eq!(
            context_ayya
                .get_meta(b"key2")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from meta cf"),
            b"ayyavalue2"
        );
        assert_eq!(
            context_ayyb
                .get_meta(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from meta cf"),
            b"ayybvalue1"
        );
    }

    #[test]
    fn test_default_cf_methods() {
        let storage = TempStorage::new();
        let tx = storage.start_transaction();
        let context_ayya = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx)
            .unwrap();
        let context_ayyb = storage
            .get_immediate_storage_context([b"ayyb"].as_ref().into(), &tx)
            .unwrap();

        context_ayya
            .put(b"key1", b"ayyavalue1", None, None)
            .unwrap()
            .expect("cannot insert into storage");
        context_ayya
            .put(b"key2", b"ayyavalue2", None, None)
            .unwrap()
            .expect("cannot insert into storage");
        context_ayyb
            .put(b"key1", b"ayybvalue1", None, None)
            .unwrap()
            .expect("cannot insert into storage");
        context_ayyb
            .put(b"key2", b"ayybvalue2", None, None)
            .unwrap()
            .expect("cannot insert into storage");

        assert_eq!(
            context_ayya
                .get(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from storage"),
            b"ayyavalue1"
        );

        context_ayya
            .delete(b"key1", None)
            .unwrap()
            .expect("cannot delete from storage");

        assert!(context_ayya
            .get(b"key1")
            .unwrap()
            .expect("cannot get from storage")
            .is_none());
        assert_eq!(
            context_ayya
                .get(b"key2")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from storage"),
            b"ayyavalue2"
        );
        assert_eq!(
            context_ayyb
                .get(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from storage"),
            b"ayybvalue1"
        );
    }

    #[test]
    fn test_batch() {
        let storage = TempStorage::new();
        let tx = storage.start_transaction();
        let context_ayya = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx)
            .unwrap();

        context_ayya
            .put(b"key1", b"ayyavalue1", None, None)
            .unwrap()
            .expect("cannot insert into storage");
        context_ayya
            .put(b"key2", b"ayyavalue2", None, None)
            .unwrap()
            .expect("cannot insert into storage");

        assert!(context_ayya
            .get(b"key3")
            .unwrap()
            .expect("cannot get from storage")
            .is_none());

        let mut batch = context_ayya.new_batch();
        batch.delete(b"key1", None);
        batch.put(b"key3", b"ayyavalue3", None, None).unwrap();

        assert!(context_ayya
            .get(b"key1")
            .unwrap()
            .expect("cannot get from storage")
            .is_some());

        context_ayya
            .commit_batch(batch)
            .unwrap()
            .expect("cannot commit a batch");

        assert!(context_ayya
            .get(b"key1")
            .unwrap()
            .expect("cannot get from storage")
            .is_none());

        storage
            .commit_transaction(tx)
            .unwrap()
            .expect("cannot commit transaction");

        let tx = storage.start_transaction();
        let context_ayya = storage
            .get_immediate_storage_context([b"ayya"].as_ref().into(), &tx)
            .unwrap();
        assert_eq!(
            context_ayya
                .get(b"key3")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from storage"),
            b"ayyavalue3"
        );
        assert!(context_ayya
            .get(b"key1")
            .unwrap()
            .expect("cannot get from storage")
            .is_none());
    }

    #[test]
    fn test_raw_iterator() {
        let storage = TempStorage::new();
        let tx = storage.start_transaction();
        let context = storage
            .get_immediate_storage_context([b"someprefix"].as_ref().into(), &tx)
            .unwrap();

        context
            .put(b"key1", b"value1", None, None)
            .unwrap()
            .expect("expected successful insertion");
        context
            .put(b"key0", b"value0", None, None)
            .unwrap()
            .expect("expected successful insertion");
        context
            .put(b"key3", b"value3", None, None)
            .unwrap()
            .expect("expected successful insertion");
        context
            .put(b"key2", b"value2", None, None)
            .unwrap()
            .expect("expected successful insertion");

        // Other storages are required to put something into rocksdb with other prefix
        // to see if there will be any conflicts and boundaries are met
        let context_before = storage
            .get_immediate_storage_context([b"anothersomeprefix"].as_ref().into(), &tx)
            .unwrap();
        context_before
            .put(b"key1", b"value1", None, None)
            .unwrap()
            .expect("expected successful insertion");
        context_before
            .put(b"key5", b"value5", None, None)
            .unwrap()
            .expect("expected successful insertion");
        let context_after = storage
            .get_immediate_storage_context([b"zanothersomeprefix"].as_ref().into(), &tx)
            .unwrap();
        context_after
            .put(b"key1", b"value1", None, None)
            .unwrap()
            .expect("expected successful insertion");
        context_after
            .put(b"key5", b"value5", None, None)
            .unwrap()
            .expect("expected successful insertion");

        let _ = storage.commit_transaction(tx).unwrap();

        // Test uncommited changes
        {
            let tx = storage.start_transaction();
            let context_tx = storage
                .get_immediate_storage_context([b"someprefix"].as_ref().into(), &tx)
                .unwrap();

            context_tx
                .delete(b"key1", None)
                .unwrap()
                .expect("unable to delete an item");
            context_tx
                .put(b"key4", b"value4", None, None)
                .unwrap()
                .expect("unable to insert an item");

            let expected: [(&'static [u8], &'static [u8]); 4] = [
                (b"key0", b"value0"),
                (b"key2", b"value2"),
                (b"key3", b"value3"),
                (b"key4", b"value4"),
            ];
            let mut expected_iter = expected.into_iter();

            // Test iterator goes forward

            let mut iter = context_tx.raw_iter();
            iter.seek_to_first().unwrap();
            while iter.valid().unwrap() {
                assert_eq!(
                    (iter.key().unwrap().unwrap(), iter.value().unwrap().unwrap()),
                    expected_iter.next().unwrap()
                );
                iter.next().unwrap();
            }
            assert!(expected_iter.next().is_none());

            // Test `seek_to_last` on a storage_cost with elements

            let mut iter = context_tx.raw_iter();
            iter.seek_to_last().unwrap();
            assert_eq!(
                (iter.key().unwrap().unwrap(), iter.value().unwrap().unwrap()),
                expected.last().unwrap().clone(),
            );
            iter.next().unwrap();
            assert!(!iter.valid().unwrap());
        }

        // Test commited data stay intact
        {
            let expected: [(&'static [u8], &'static [u8]); 4] = [
                (b"key0", b"value0"),
                (b"key1", b"value1"),
                (b"key2", b"value2"),
                (b"key3", b"value3"),
            ];
            let mut expected_iter = expected.into_iter();
            let tx = storage.start_transaction();
            let context = storage
                .get_immediate_storage_context([b"someprefix"].as_ref().into(), &tx)
                .unwrap();

            let mut iter = context.raw_iter();
            iter.seek_to_first().unwrap();
            while iter.valid().unwrap() {
                assert_eq!(
                    (iter.key().unwrap().unwrap(), iter.value().unwrap().unwrap()),
                    expected_iter.next().unwrap()
                );
                iter.next().unwrap();
            }
            assert!(expected_iter.next().is_none());
        }
    }
}

mod batch_no_transaction {
    use super::*;
    use crate::{Batch, Storage, StorageBatch, StorageContext};

    #[test]
    fn test_various_cf_methods() {
        let storage = TempStorage::new();
        let batch = StorageBatch::new();
        let transaction = storage.start_transaction();

        let context_ayya = storage
            .get_transactional_storage_context(
                [b"ayya"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        let context_ayyb = storage
            .get_transactional_storage_context(
                [b"ayyb"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();

        context_ayya
            .put_aux(b"key1", b"ayyavalue1", None)
            .unwrap()
            .expect("cannot insert into aux cf");
        context_ayya
            .put_meta(b"key2", b"ayyavalue2", None)
            .unwrap()
            .expect("cannot insert into meta cf");
        context_ayya
            .put_root(b"key3", b"ayyavalue3", None)
            .unwrap()
            .expect("cannot insert into roots cf");
        context_ayya
            .put(b"key4", b"ayyavalue4", None, None)
            .unwrap()
            .expect("cannot insert data");
        context_ayyb
            .put_aux(b"key1", b"ayybvalue1", None)
            .unwrap()
            .expect("cannot insert into aux cf");
        context_ayyb
            .put_meta(b"key2", b"ayybvalue2", None)
            .unwrap()
            .expect("cannot insert into meta cf");
        context_ayyb
            .put_root(b"key3", b"ayybvalue3", None)
            .unwrap()
            .expect("cannot insert into roots cf");
        context_ayyb
            .put(b"key4", b"ayybvalue4", None, None)
            .unwrap()
            .expect("cannot insert data");

        // There is no "staging" data for batch contexts: `get` will access only
        // pre-batch data (thus `None` until commit).
        assert!(context_ayya
            .get_aux(b"key1")
            .unwrap()
            .expect("cannot get from aux cf")
            .is_none());

        assert_eq!(batch.len(), 8);

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("cannot commit batch");

        let context_ayya = storage
            .get_transactional_storage_context([b"ayya"].as_ref().into(), None, &transaction)
            .unwrap();
        let context_ayyb = storage
            .get_transactional_storage_context([b"ayyb"].as_ref().into(), None, &transaction)
            .unwrap();

        assert_eq!(
            context_ayya
                .get_aux(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from aux cf"),
            b"ayyavalue1",
        );
        assert_eq!(
            context_ayya
                .get_meta(b"key2")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from meta cf"),
            b"ayyavalue2",
        );
        assert_eq!(
            context_ayya
                .get_root(b"key3")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from roots cf"),
            b"ayyavalue3",
        );
        assert_eq!(
            context_ayya
                .get(b"key4")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get data"),
            b"ayyavalue4",
        );

        assert_eq!(
            context_ayyb
                .get_aux(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from aux cf"),
            b"ayybvalue1",
        );
        assert_eq!(
            context_ayyb
                .get_meta(b"key2")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from meta cf"),
            b"ayybvalue2",
        );
        assert_eq!(
            context_ayyb
                .get_root(b"key3")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get from roots cf"),
            b"ayybvalue3",
        );
        assert_eq!(
            context_ayyb
                .get(b"key4")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get data"),
            b"ayybvalue4",
        );
    }

    #[test]
    fn test_with_db_batches() {
        let storage = TempStorage::new();
        let batch = StorageBatch::new();
        let transaction = storage.start_transaction();

        let context_ayya = storage
            .get_transactional_storage_context(
                [b"ayya"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        let context_ayyb = storage
            .get_transactional_storage_context(
                [b"ayyb"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();

        context_ayya
            .put(b"key1", b"ayyavalue1", None, None)
            .unwrap()
            .expect("cannot insert data");
        let mut db_batch_ayya = context_ayya.new_batch();
        db_batch_ayya
            .put(b"key2", b"ayyavalue2", None, None)
            .expect("should not error");
        db_batch_ayya
            .put(b"key3", b"ayyavalue3", None, None)
            .expect("should not error");

        context_ayyb
            .put(b"key1", b"ayybvalue1", None, None)
            .unwrap()
            .expect("cannot insert data");
        let mut db_batch_ayyb = context_ayyb.new_batch();
        db_batch_ayyb
            .put(b"key2", b"ayybvalue2", None, None)
            .expect("should not error");
        db_batch_ayyb
            .put(b"key3", b"ayybvalue3", None, None)
            .expect("should not error");

        // DB batches are not commited yet, so these operations are missing from
        // StorageBatch
        assert_eq!(batch.len(), 2);

        context_ayya
            .commit_batch(db_batch_ayya)
            .unwrap()
            .expect("cannot commit db batch");
        context_ayyb
            .commit_batch(db_batch_ayyb)
            .unwrap()
            .expect("cannot commit db batch");

        // DB batches are "commited", but actually staged in multi-context batch to do
        // it in a single run to the database
        assert_eq!(batch.len(), 6);

        assert!(context_ayya
            .get(b"key1")
            .unwrap()
            .expect("cannot get data")
            .is_none());
        assert!(context_ayya
            .get(b"key3")
            .unwrap()
            .expect("cannot get data")
            .is_none());

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("cannot commit multi context batch");

        let context_ayya = storage
            .get_transactional_storage_context([b"ayya"].as_ref().into(), None, &transaction)
            .unwrap();
        assert_eq!(
            context_ayya
                .get(b"key3")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get data"),
            b"ayyavalue3"
        );
    }
}

mod batch_transaction {
    use super::*;
    use crate::{Batch, RawIterator, Storage, StorageBatch, StorageContext};

    #[test]
    fn test_transaction_properties() {
        let storage = TempStorage::new();
        let other_transaction = storage.start_transaction();
        let transaction = storage.start_transaction();

        let batch = StorageBatch::new();
        let batch_tx = StorageBatch::new();
        let context_ayya = storage
            .get_transactional_storage_context(
                [b"ayya"].as_ref().into(),
                Some(&batch),
                &other_transaction,
            )
            .unwrap();
        let context_ayyb = storage
            .get_transactional_storage_context(
                [b"ayyb"].as_ref().into(),
                Some(&batch),
                &other_transaction,
            )
            .unwrap();
        let context_ayya_tx = storage
            .get_transactional_storage_context(
                [b"ayya"].as_ref().into(),
                Some(&batch_tx),
                &transaction,
            )
            .unwrap();
        let context_ayyb_tx = storage
            .get_transactional_storage_context(
                [b"ayyb"].as_ref().into(),
                Some(&batch_tx),
                &transaction,
            )
            .unwrap();

        // Data should be visible in transaction...
        context_ayya_tx
            .put(b"key1", b"ayyavalue1", None, None)
            .unwrap()
            .expect("cannot insert data");
        context_ayyb_tx
            .put(b"key1", b"ayybvalue1", None, None)
            .unwrap()
            .expect("cannot insert data");

        storage
            .commit_multi_context_batch(batch_tx, Some(&transaction))
            .unwrap()
            .expect("cannot commit a non-tx multi context batch");

        let another_batch_tx = StorageBatch::new();
        let context_ayya_tx = storage
            .get_transactional_storage_context(
                [b"ayya"].as_ref().into(),
                Some(&another_batch_tx),
                &transaction,
            )
            .unwrap();
        let context_ayyb_tx = storage
            .get_transactional_storage_context(
                [b"ayyb"].as_ref().into(),
                Some(&another_batch_tx),
                &transaction,
            )
            .unwrap();

        assert_eq!(
            context_ayya_tx
                .get(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get data"),
            b"ayyavalue1"
        );
        assert_eq!(
            context_ayyb_tx
                .get(b"key1")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get data"),
            b"ayybvalue1"
        );

        // ...but not outside of it
        assert!(context_ayya
            .get(b"key1")
            .unwrap()
            .expect("cannot get data")
            .is_none());
        assert!(context_ayyb
            .get(b"key1")
            .unwrap()
            .expect("cannot get data")
            .is_none());

        // Batches data won't be visible either in transaction and outside of it until
        // batch is commited

        let batch = StorageBatch::new();
        let context_ayya_batch = storage
            .get_transactional_storage_context(
                [b"ayya"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        let context_ayyb_batch = storage
            .get_transactional_storage_context(
                [b"ayyb"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        context_ayya_batch
            .put_aux(b"key2", b"ayyavalue2", None)
            .unwrap()
            .expect("cannot put aux data into batch");
        context_ayyb_batch
            .put_aux(b"key2", b"ayybvalue2", None)
            .unwrap()
            .expect("cannot put aux data into batch");

        assert_eq!(batch.len(), 2);

        assert!(context_ayya_tx
            .get_aux(b"key2")
            .unwrap()
            .expect("cannot get data")
            .is_none());
        assert!(context_ayyb_tx
            .get_aux(b"key2")
            .unwrap()
            .expect("cannot get data")
            .is_none());
        assert!(context_ayya
            .get_aux(b"key2")
            .unwrap()
            .expect("cannot get data")
            .is_none());
        assert!(context_ayyb
            .get_aux(b"key2")
            .unwrap()
            .expect("cannot get data")
            .is_none());

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("cannot commit batch");

        // Commited batch data is accessible in transaction but not outside
        assert_eq!(
            context_ayya_tx
                .get_aux(b"key2")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get data"),
            b"ayyavalue2"
        );

        assert!(context_ayya
            .get_aux(b"key2")
            .unwrap()
            .expect("cannot get data")
            .is_none());

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("cannot commit transaction");

        assert_eq!(
            context_ayya
                .get_aux(b"key2")
                .unwrap()
                .ok()
                .flatten()
                .expect("cannot get data"),
            b"ayyavalue2"
        );
    }

    #[test]
    fn test_db_batch_in_transaction_merged_into_context_batch() {
        let storage = TempStorage::new();
        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();

        let context_ayya = storage
            .get_transactional_storage_context(
                [b"ayya"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        let context_ayyb = storage
            .get_transactional_storage_context(
                [b"ayyb"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();

        let mut db_batch_a = context_ayya.new_batch();
        let mut db_batch_b = context_ayyb.new_batch();

        db_batch_a.put(b"key1", b"value1", None, None).unwrap();
        db_batch_b.put(b"key2", b"value2", None, None).unwrap();

        // Until db batches are commited our multi-context batch should be empty
        assert_eq!(batch.len(), 0);

        context_ayya
            .commit_batch(db_batch_a)
            .unwrap()
            .expect("cannot commit batch");
        context_ayya
            .commit_batch(db_batch_b)
            .unwrap()
            .expect("cannot commit batch");

        // All operations are in multi-context batch, but not visible in DB yet
        assert_eq!(batch.len(), 2);
        assert!(context_ayya
            .get(b"key1")
            .unwrap()
            .expect("cannot get data")
            .is_none());
        assert!(context_ayyb
            .get(b"key2")
            .unwrap()
            .expect("cannot get data")
            .is_none());

        // Commited batch's data should be visible in transaction
        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("cannot commit multi-context batch");

        // Obtaining new contexts outside a commited batch but still within a
        // transaction
        let context_ayya = storage
            .get_transactional_storage_context([b"ayya"].as_ref().into(), None, &transaction)
            .unwrap();
        let context_ayyb = storage
            .get_transactional_storage_context([b"ayyb"].as_ref().into(), None, &transaction)
            .unwrap();

        assert_eq!(
            context_ayya.get(b"key1").unwrap().expect("cannot get data"),
            Some(b"value1".to_vec())
        );
        assert_eq!(
            context_ayyb.get(b"key2").unwrap().expect("cannot get data"),
            Some(b"value2".to_vec())
        );

        // And still no data in the database until transaction is commited
        let other_transaction = storage.start_transaction();
        let context_ayya = storage
            .get_transactional_storage_context([b"ayya"].as_ref().into(), None, &other_transaction)
            .unwrap();
        let context_ayyb = storage
            .get_transactional_storage_context([b"ayyb"].as_ref().into(), None, &other_transaction)
            .unwrap();

        let mut iter = context_ayya.raw_iter();
        iter.seek_to_first().unwrap();
        assert!(!iter.valid().unwrap());

        let mut iter = context_ayyb.raw_iter();
        iter.seek_to_first().unwrap();
        assert!(!iter.valid().unwrap());

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("cannot commit transaction");

        let other_transaction = storage.start_transaction();
        let context_ayya = storage
            .get_transactional_storage_context([b"ayya"].as_ref().into(), None, &other_transaction)
            .unwrap();
        let context_ayyb = storage
            .get_transactional_storage_context([b"ayyb"].as_ref().into(), None, &other_transaction)
            .unwrap();

        assert_eq!(
            context_ayya.get(b"key1").unwrap().expect("cannot get data"),
            Some(b"value1".to_vec())
        );
        assert_eq!(
            context_ayyb.get(b"key2").unwrap().expect("cannot get data"),
            Some(b"value2".to_vec())
        );
    }
}
