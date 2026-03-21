use grovedb_costs::{cost_return_on_error, CostResult, CostsExt};
use grovedb_merk::{
    element::{
        costs::ElementCostExtensions, get::ElementFetchFromStorageExtensions,
        tree_type::ElementTreeTypeExtensions,
    },
    Merk, TreeType,
};
use grovedb_path::SubtreePath;
use grovedb_storage::{
    rocksdb_storage::{PrefixedRocksDbTransactionContext, RocksDbStorage},
    Storage, StorageBatch,
};
use grovedb_version::version::GroveVersion;

use crate::{Element, Error, Transaction};

pub(crate) trait OpenMerkErrorsCompat {
    fn parent_key_not_found<B: AsRef<[u8]>>(
        e: Error,
        parent_path: SubtreePath<B>,
        parent_key: &[u8],
    ) -> Error;

    fn open_base_error() -> Error;

    fn parent_must_be_tree() -> Error;
}

pub(crate) fn open_merk<'db, 'b, B, C: OpenMerkErrorsCompat>(
    db: &'db RocksDbStorage,
    path: SubtreePath<'b, B>,
    tx: &'db Transaction,
    batch: Option<&'db StorageBatch>,
    grove_version: &GroveVersion,
) -> CostResult<Merk<PrefixedRocksDbTransactionContext<'db>>, Error>
where
    B: AsRef<[u8]> + 'b,
{
    let mut cost = Default::default();

    let storage = db
        .get_transactional_storage_context(path.clone(), batch, tx)
        .unwrap_add_cost(&mut cost);
    if let Some((parent_path, parent_key)) = path.derive_parent() {
        let parent_storage = db
            .get_transactional_storage_context(parent_path.clone(), batch, tx)
            .unwrap_add_cost(&mut cost);
        let element = cost_return_on_error!(
            &mut cost,
            Element::get_from_storage(&parent_storage, parent_key, grove_version)
                .map_err(|e| C::parent_key_not_found(e.into(), parent_path, parent_key))
        );
        if let Some((root_key, tree_type)) = element.root_key_and_tree_type_owned() {
            Merk::open_layered_with_root_key(
                storage,
                root_key,
                tree_type,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .map_err(|_| C::parent_must_be_tree())
            .add_cost(cost)
        } else {
            Err(Error::CorruptedPath(
                "cannot open a subtree as parent exists but is not a tree".to_string(),
            ))
            .wrap_with_cost(cost)
        }
    } else {
        Merk::open_base(
            storage,
            TreeType::NormalTree,
            Some(&Element::value_defined_cost_for_serialized_value),
            grove_version,
        )
        .map_err(|_| C::open_base_error())
        .add_cost(cost)
    }
}

/// Opens a subtree with errors returned compatible to now removed
/// `merk_optional_tx!` macro.
pub(crate) fn merk_optional_tx<'db, 'b, B>(
    db: &'db RocksDbStorage,
    path: SubtreePath<'b, B>,
    tx: &'db Transaction,
    batch: Option<&'db StorageBatch>,
    grove_version: &GroveVersion,
) -> CostResult<Merk<PrefixedRocksDbTransactionContext<'db>>, Error>
where
    B: AsRef<[u8]> + 'b,
{
    struct Compat;

    impl OpenMerkErrorsCompat for Compat {
        fn parent_key_not_found<B: AsRef<[u8]>>(
            e: Error,
            _parent_path: SubtreePath<B>,
            _parent_key: &[u8],
        ) -> Error {
            Error::PathParentLayerNotFound(format!(
                "could not get key for parent of subtree optional on tx: {}",
                e
            ))
        }

        fn open_base_error() -> Error {
            Error::CorruptedData("cannot open a subtree".to_owned())
        }

        fn parent_must_be_tree() -> Error {
            Error::CorruptedData("parent is not a tree".to_owned())
        }
    }

    open_merk::<_, Compat>(db, path, tx, batch, grove_version)
}

/// Opens a subtree with errors returned compatible to now removed
/// `merk_optional_tx_path_not_empty!` macro.
pub(crate) fn merk_optional_tx_path_not_empty<'db, 'b, B>(
    db: &'db RocksDbStorage,
    path: SubtreePath<'b, B>,
    tx: &'db Transaction,
    batch: Option<&'db StorageBatch>,
    grove_version: &GroveVersion,
) -> CostResult<Merk<PrefixedRocksDbTransactionContext<'db>>, Error>
where
    B: AsRef<[u8]> + 'b,
{
    if path.is_root() {
        Err(Error::CorruptedData("path is empty".to_owned())).wrap_with_cost(Default::default())
    } else {
        merk_optional_tx(db, path, tx, batch, grove_version)
    }
}
