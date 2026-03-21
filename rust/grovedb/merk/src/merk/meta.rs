//! Metadata access for Merk trees

use std::collections::hash_map::Entry;

use grovedb_costs::{CostResult, CostsExt};
use grovedb_storage::StorageContext;

use super::Merk;
use crate::Error;

impl<'db, S: StorageContext<'db>> Merk<S> {
    /// Get metadata for the Merk under `key`.
    pub fn get_meta(&mut self, key: Vec<u8>) -> CostResult<Option<&[u8]>, Error> {
        match self.meta_cache.entry(key) {
            Entry::Occupied(e) => Ok(e.into_mut().as_deref()).wrap_with_cost(Default::default()),
            Entry::Vacant(e) => self
                .storage
                .get_meta(e.key())
                .map_ok(|b| e.insert(b).as_deref())
                .map_err(Error::StorageError),
        }
    }

    /// Set metadata under `key`. This doesn't affect the state (root hash).
    pub fn put_meta(&mut self, key: Vec<u8>, value: Vec<u8>) -> CostResult<(), Error> {
        self.storage
            .put_meta(&key, &value, None)
            .map_ok(|_| {
                self.meta_cache.insert(key, Some(value));
            })
            .map_err(Error::StorageError)
    }

    /// Delete metadata under `key`.
    pub fn delete_meta(&mut self, key: &[u8]) -> CostResult<(), Error> {
        self.storage
            .delete_meta(key, None)
            .map_ok(|_| {
                self.meta_cache.remove(key);
            })
            .map_err(Error::StorageError)
    }
}

#[cfg(test)]
mod tests {
    use grovedb_costs::OperationCost;
    use grovedb_version::version::GroveVersion;

    use crate::test_utils::TempMerk;

    #[test]
    fn meta_storage_data_retrieval() {
        let version = GroveVersion::latest();
        let mut merk = TempMerk::new(&version);

        merk.put_meta(b"key".to_vec(), b"value".to_vec())
            .unwrap()
            .unwrap();

        let mut cost: OperationCost = Default::default();
        assert_eq!(
            merk.get_meta(b"key".to_vec())
                .unwrap_add_cost(&mut cost)
                .unwrap(),
            Some(b"value".as_slice())
        );
        assert!(cost.is_nothing());
    }

    #[test]
    fn meta_storage_works_uncommited() {
        let version = GroveVersion::latest();
        let mut merk = TempMerk::new(&version);

        let mut cost_1: OperationCost = Default::default();
        assert!(merk
            .get_meta(b"key".to_vec())
            .unwrap_add_cost(&mut cost_1)
            .unwrap()
            .is_none());
        assert!(!cost_1.is_nothing());

        let mut cost_2: OperationCost = Default::default();
        assert!(merk
            .get_meta(b"key".to_vec())
            .unwrap_add_cost(&mut cost_2)
            .unwrap()
            .is_none());
        assert!(cost_2.is_nothing());
    }

    #[test]
    fn meta_storage_deletion() {
        let version = GroveVersion::latest();
        let mut merk = TempMerk::new(&version);

        merk.put_meta(b"key".to_vec(), b"value".to_vec())
            .unwrap()
            .unwrap();

        assert_eq!(
            merk.get_meta(b"key".to_vec()).unwrap().unwrap(),
            Some(b"value".as_slice())
        );

        merk.delete_meta(b"key").unwrap().unwrap();

        assert!(merk.get_meta(b"key".to_vec()).unwrap().unwrap().is_none());
    }
}
