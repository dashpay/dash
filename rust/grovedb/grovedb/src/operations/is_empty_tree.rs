//! Check if empty tree operations

use grovedb_costs::{cost_return_on_error, CostResult, OperationCost};
use grovedb_path::SubtreePath;
use grovedb_version::{check_grovedb_v0_with_cost, version::GroveVersion};

use crate::{
    util::{compat, TxRef},
    Error, GroveDb, TransactionArg,
};

impl GroveDb {
    /// Check if it's an empty tree
    pub fn is_empty_tree<'b, B, P>(
        &self,
        path: P,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<bool, Error>
    where
        B: AsRef<[u8]> + 'b,
        P: Into<SubtreePath<'b, B>>,
    {
        check_grovedb_v0_with_cost!(
            "is_empty_tree",
            grove_version.grovedb_versions.operations.get.is_empty_tree
        );
        let mut cost = OperationCost::default();
        let path: SubtreePath<B> = path.into();

        let tx = TxRef::new(&self.db, transaction);

        cost_return_on_error!(
            &mut cost,
            self.check_subtree_exists_path_not_found(path.clone(), tx.as_ref(), grove_version)
        );
        let subtree = cost_return_on_error!(
            &mut cost,
            compat::merk_optional_tx(&self.db, path, tx.as_ref(), None, grove_version)
        );

        subtree.is_empty_tree().add_cost(cost).map(Ok)
    }
}
