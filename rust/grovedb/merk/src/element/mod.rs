use grovedb_costs::OperationCost;
use grovedb_element::{error::ElementError, Element};

use crate::tree::value_hash;

#[cfg(feature = "minimal")]
pub mod costs;
#[cfg(feature = "minimal")]
pub mod decode;
#[cfg(feature = "minimal")]
pub mod delete;
#[cfg(feature = "minimal")]
pub mod exists;
#[cfg(feature = "minimal")]
pub mod get;
#[cfg(feature = "minimal")]
pub mod insert;
pub mod tree_type;

pub trait ElementExt {
    fn value_hash(
        &self,
        grove_version: &grovedb_version::version::GroveVersion,
    ) -> grovedb_costs::CostResult<[u8; 32], ElementError>;
}

impl ElementExt for Element {
    fn value_hash(
        &self,
        grove_version: &grovedb_version::version::GroveVersion,
    ) -> grovedb_costs::CostResult<[u8; 32], ElementError> {
        let bytes = grovedb_costs::cost_return_on_error_default!(self.serialize(grove_version));
        value_hash(&bytes).map(Ok)
    }
}
