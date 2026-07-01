use grovedb_element::Element;
use grovedb_version::version::GroveVersion;

use crate::{element::costs::ElementCostExtensions, tree::TreeNode, Error};

pub trait ElementDecodeExtensions {
    /// Decode from bytes
    fn raw_decode(bytes: &[u8], grove_version: &GroveVersion) -> Result<Element, Error>;
}

impl ElementDecodeExtensions for Element {
    /// Decode from bytes
    fn raw_decode(bytes: &[u8], grove_version: &GroveVersion) -> Result<Element, Error> {
        let tree = TreeNode::decode_raw(
            bytes,
            vec![],
            Some(Element::value_defined_cost_for_serialized_value),
            grove_version,
        )
        .map_err(|e| Error::CorruptedData(e.to_string()))?;
        let element: Element = Element::deserialize(tree.value_as_slice(), grove_version)?;
        Ok(element)
    }
}
