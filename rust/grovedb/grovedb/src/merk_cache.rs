//! Module dedicated to keep necessary Merks in memory.

use std::{
    cell::{Cell, UnsafeCell},
    collections::{btree_map::Entry, BTreeMap},
};

use grovedb_costs::{cost_return_on_error, CostResult, CostsExt};
use grovedb_merk::Merk;
use grovedb_path::SubtreePathBuilder;
use grovedb_storage::{rocksdb_storage::PrefixedRocksDbTransactionContext, StorageBatch};
use grovedb_version::version::GroveVersion;

use crate::{Error, GroveDb, Transaction};

type TxMerk<'db> = Merk<PrefixedRocksDbTransactionContext<'db>>;

/// We store Merk on heap to preserve its location as well as borrow flag
/// alongside.
type CachedMerkEntry<'db> = Box<(Cell<bool>, TxMerk<'db>)>;

/// Structure to keep subtrees open in memory for repeated access.
pub(crate) struct MerkCache<'db, 'b, B: AsRef<[u8]>> {
    db: &'db GroveDb,
    pub(crate) version: &'db GroveVersion,
    batch: Box<StorageBatch>,
    tx: &'db Transaction<'db>,
    merks: UnsafeCell<BTreeMap<SubtreePathBuilder<'b, B>, CachedMerkEntry<'db>>>,
}

impl<'db, 'b, B: AsRef<[u8]>> MerkCache<'db, 'b, B> {
    /// Initialize a new `MerkCache` instance
    pub(crate) fn new(
        db: &'db GroveDb,
        tx: &'db Transaction<'db>,
        version: &'db GroveVersion,
    ) -> Self {
        MerkCache {
            db,
            tx,
            version,
            merks: Default::default(),
            batch: Default::default(),
        }
    }

    /// Gets a smart pointer to a cached Merk or opens one if needed.
    pub(crate) fn get_merk<'c>(
        &'c self,
        path: SubtreePathBuilder<'b, B>,
    ) -> CostResult<MerkHandle<'db, 'c>, Error> {
        let mut cost = Default::default();

        // SAFETY: there are no other references to `merks` memory at the same time.
        // Note while it's possible to have direct references to actual Merk trees,
        // outside of the scope of this function, this map (`merks`) has
        // indirect connection to them through `Box`, thus there are no overlapping
        // references, and that is requirement of `UnsafeCell` we have there.
        let boxed_flag_merk = match unsafe {
            self.merks
                .get()
                .as_mut()
                .expect("`UnsafeCell` is never null")
        }
        .entry(path)
        {
            Entry::Vacant(e) => {
                let merk = cost_return_on_error!(
                    &mut cost,
                    self.db.open_transactional_merk_at_path(
                        e.key().into(),
                        self.tx,
                        // SAFETY: batch is allocated on the heap and we use only shared
                        // references, so as long as the `Box` allocation
                        // outlives those references we're safe,
                        // and it will outlive because Merks are dropped first.
                        Some(unsafe {
                            (&*self.batch as *const StorageBatch)
                                .as_ref()
                                .expect("`Box` is never null")
                        }),
                        self.version
                    )
                );
                e.insert(Box::new((false.into(), merk)))
            }
            Entry::Occupied(e) => e.into_mut(),
        };

        let taken_handle_ref: *const Cell<bool> = &boxed_flag_merk.0 as *const _;
        let merk_ptr: *mut TxMerk<'db> = &mut boxed_flag_merk.1 as *mut _;

        // SAFETY: `MerkHandle` contains two references to the heap allocated memory,
        // and we want to be sure that the referenced data will outlive those
        // references plus borrowing rules aren't violated (one `&mut` or many
        // `&` with no `&mut` at a time).
        //
        // To make sure changes to the map won't affect existing borrows we have an
        // indirection in a form of `Box`, that allows us to move and update
        // `MerkCache` with new subtrees and possible reallocations without breaking
        // `MerkHandle`'s references. We use `UnsafeCell` to connect lifetimes and check
        // in compile time that `MerkHandle`s won't outlive the cache, even though we
        // don't hold any references to it, but `&mut` reference would make this borrow
        // exclusive for the whole time of `MerkHandle`, so it shall go intially through
        // a shared reference.
        //
        // Borrowing rules are covered using a borrow flag of each Merk:
        // 1. Borrow flag's reference points to a heap allocated memory and will remain
        //    valid. Since the reference is shared and no need to obtain a `&mut`
        //    reference this part of the memory is covered.
        // 2. For the same reason the Merk's pointer can be converted to a reference,
        //    because the memory behind the `Box` is valid and `MerkHandle` can't
        //    outlive it since we use lifetime parameters.
        // 3. We can get unique reference out of that pointer safely because of
        //    borrowing flag.
        Ok(unsafe {
            MerkHandle {
                merk: merk_ptr,
                taken_handle: taken_handle_ref
                    .as_ref()
                    .expect("`Box` contents are never null"),
            }
        })
        .wrap_with_cost(cost)
    }

    /// Consumes `MerkCache` into accumulated batch of uncommited operations
    /// with subtrees' root hash  propagation done.
    pub(crate) fn into_batch(mut self) -> CostResult<Box<StorageBatch>, Error> {
        let mut cost = Default::default();
        cost_return_on_error!(&mut cost, self.propagate_subtrees());

        // SAFETY: By this time all subtrees are taken and dropped during
        // propagation, so there are no more references to the batch and in can be
        // safely released into the world.
        Ok(self.batch).wrap_with_cost(cost)
    }

    fn propagate_subtrees(&mut self) -> CostResult<(), Error> {
        let mut cost = Default::default();

        // This relies on [SubtreePath]'s ordering implementation to put the deepest
        // path's first.
        while let Some((path, flag_and_merk)) = self.merks.get_mut().pop_first() {
            let merk = flag_and_merk.1;
            if let Some((parent_path, parent_key)) = path.derive_parent_owned() {
                let mut parent_merk = cost_return_on_error!(&mut cost, self.get_merk(parent_path));

                let (root_hash, root_key, aggregate_data) = cost_return_on_error!(
                    &mut cost,
                    merk.root_hash_key_and_aggregate_data()
                        .map_err(Error::MerkError)
                );
                cost_return_on_error!(
                    &mut cost,
                    parent_merk.for_merk(|m| GroveDb::update_tree_item_preserve_flag(
                        m,
                        parent_key,
                        root_key,
                        root_hash,
                        aggregate_data,
                        self.version,
                    ))
                );
            }
        }

        Ok(()).wrap_with_cost(cost)
    }
}

/// Wrapper over `Merk` tree to manage unqiue borrow dynamically.
#[derive(Clone)]
pub(crate) struct MerkHandle<'db, 'c> {
    merk: *mut TxMerk<'db>,
    taken_handle: &'c Cell<bool>,
}

impl<'db> MerkHandle<'db, '_> {
    pub(crate) fn for_merk<T>(&mut self, f: impl FnOnce(&mut TxMerk<'db>) -> T) -> T {
        if self.taken_handle.get() {
            panic!("Attempt to have double &mut borrow on Merk");
        }

        self.taken_handle.set(true);

        // SAFETY: here we want to have `&mut` reference to Merk out of a pointer, there
        // is a checklist for that:
        // 1. Memory is valid, because `MerkHandle` can't outlive `MerkCache` and heap
        //    allocated Merks stay at their place for the whole `MerkCache` lifetime.
        // 2. No other references exist because of `taken_handle` check above.
        let result = f(unsafe { self.merk.as_mut().expect("`Box` contents are never null") });

        self.taken_handle.set(false);

        result
    }
}

#[cfg(test)]
mod tests {
    use grovedb_merk::element::insert::ElementInsertToStorageExtensions;
    use grovedb_path::SubtreePath;
    use grovedb_storage::StorageBatch;
    use grovedb_version::version::GroveVersion;

    use super::MerkCache;
    use crate::{
        tests::{make_deep_tree, make_test_grovedb, TEST_LEAF},
        Element,
    };

    #[test]
    #[should_panic]
    fn cant_borrow_twice() {
        let version = GroveVersion::latest();
        let db = make_test_grovedb(&version);
        let tx = db.start_transaction();

        let cache = MerkCache::new(&db, &tx, version);

        let mut merk1 = cache
            .get_merk(SubtreePath::empty().derive_owned())
            .unwrap()
            .unwrap();
        let mut merk2 = cache
            .get_merk(SubtreePath::empty().derive_owned())
            .unwrap()
            .unwrap();

        merk1.for_merk(|_m1| {
            merk2.for_merk(|_m2| {
                // this shouldn't happen
            })
        });
    }

    #[test]
    fn subtrees_are_propagated() {
        let version = GroveVersion::latest();
        let db = make_deep_tree(&version);
        let tx = db.start_transaction();

        let path = SubtreePath::from(&[TEST_LEAF, b"innertree"]);
        let item = Element::new_item(b"hello".to_vec());

        let no_propagation_ops_count = {
            let batch = StorageBatch::new();

            let mut merk = db
                .open_transactional_merk_at_path(path.clone(), &tx, Some(&batch), &version)
                .unwrap()
                .unwrap();

            item.insert(&mut merk, b"k1", None, &version)
                .unwrap()
                .unwrap();

            batch.len()
        };

        let cache = MerkCache::new(&db, &tx, version);

        let mut merk = cache.get_merk(path.derive_owned()).unwrap().unwrap();

        merk.for_merk(|m| item.insert(m, b"k1", None, &version).unwrap().unwrap());

        drop(merk);

        assert!(cache.into_batch().unwrap().unwrap().len() > no_propagation_ops_count);
    }
}
