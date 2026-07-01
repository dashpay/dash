use grovedb_costs::{cost_return_on_error, CostResult, CostsExt, OperationCost};
use grovedb_storage::{Batch, RawIterator, StorageContext};

use crate::{Error, Error::StorageError, Merk};

impl<'db, S> Merk<S>
where
    S: StorageContext<'db>,
{
    /// Deletes tree data
    pub fn clear(&mut self) -> CostResult<(), Error> {
        let mut cost = OperationCost::default();

        let mut iter = self.storage.raw_iter();
        iter.seek_to_first().unwrap_add_cost(&mut cost);

        let mut to_delete = self.storage.new_batch();
        while iter.valid().unwrap_add_cost(&mut cost) {
            if let Some(key) = iter.key().unwrap_add_cost(&mut cost) {
                // todo: deal with cost reimbursement
                to_delete.delete(key, None);
            }
            iter.next().unwrap_add_cost(&mut cost);
        }
        cost_return_on_error!(
            &mut cost,
            self.storage.commit_batch(to_delete).map_err(StorageError)
        );
        self.tree.set(None);
        Ok(()).wrap_with_cost(cost)
    }
}
