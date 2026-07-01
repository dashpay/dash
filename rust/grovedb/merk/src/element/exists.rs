//! Exists
//! Implements in Element functions for checking if stuff exists

use grovedb_costs::CostResult;
use grovedb_element::Element;
use grovedb_storage::StorageContext;
use grovedb_version::{check_grovedb_v0_with_cost, version::GroveVersion};

use crate::{element::costs::ElementCostExtensions, Error, Merk};

pub trait ElementExistsInStorageExtensions {
    /// Helper function that returns whether an element at the key for the
    /// element already exists.
    fn element_at_key_already_exists<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        &self,
        merk: &mut Merk<S>,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<bool, Error>;
}

impl ElementExistsInStorageExtensions for Element {
    /// Helper function that returns whether an element at the key for the
    /// element already exists.
    fn element_at_key_already_exists<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        &self,
        merk: &mut Merk<S>,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<bool, Error> {
        check_grovedb_v0_with_cost!(
            "element_at_key_already_exists",
            grove_version
                .grovedb_versions
                .element
                .element_at_key_already_exists
        );
        merk.exists(
            key.as_ref(),
            Some(&Element::value_defined_cost_for_serialized_value),
            grove_version,
        )
        .map_err(|e| Error::CorruptedData(e.to_string()))
    }
}
