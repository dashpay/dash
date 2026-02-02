//! Serialize
//! Implements serialization functions in Element

use bincode::config;
use grovedb_version::{check_grovedb_v0, version::GroveVersion};

use crate::{element::Element, error::ElementError};

impl Element {
    /// Serializes self. Returns vector of u8s.
    pub fn serialize(&self, grove_version: &GroveVersion) -> Result<Vec<u8>, ElementError> {
        check_grovedb_v0!(
            "Element::serialize",
            grove_version.grovedb_versions.element.serialize
        );
        let config = config::standard().with_big_endian().with_no_limit();
        bincode::encode_to_vec(self, config)
            .map_err(|e| ElementError::CorruptedData(format!("unable to serialize element {}", e)))
    }

    /// Serializes self. Returns usize.
    pub fn serialized_size(&self, grove_version: &GroveVersion) -> Result<usize, ElementError> {
        check_grovedb_v0!(
            "Element::serialized_size",
            grove_version.grovedb_versions.element.serialized_size
        );
        self.serialize(grove_version)
            .map(|serialized| serialized.len())
    }

    /// Deserializes given bytes and sets as self
    pub fn deserialize(bytes: &[u8], grove_version: &GroveVersion) -> Result<Self, ElementError> {
        check_grovedb_v0!(
            "Element::deserialize",
            grove_version.grovedb_versions.element.deserialize
        );
        let config = config::standard().with_big_endian().with_no_limit();
        Ok(bincode::decode_from_slice(bytes, config)
            .map_err(|e| {
                ElementError::CorruptedData(format!("unable to deserialize element {}", e))
            })?
            .0)
    }
}

#[cfg(test)]
mod tests {
    use integer_encoding::VarInt;

    use super::*;
    use crate::reference_path::ReferencePathType;

    #[test]
    fn test_serialization() {
        let grove_version = GroveVersion::latest();
        let empty_tree = Element::empty_tree();
        let serialized = empty_tree
            .serialize(grove_version)
            .expect("expected to serialize");
        assert_eq!(serialized.len(), 3);
        assert_eq!(
            serialized.len(),
            empty_tree.serialized_size(grove_version).unwrap()
        );
        // The tree is fixed length 32 bytes, so it's enum 2 then 32 bytes of zeroes
        assert_eq!(hex::encode(serialized), "020000");

        let empty_tree = Element::new_tree_with_flags(None, Some(vec![5]));
        let serialized = empty_tree
            .serialize(grove_version)
            .expect("expected to serialize");
        assert_eq!(serialized.len(), 5);
        assert_eq!(
            serialized.len(),
            empty_tree.serialized_size(grove_version).unwrap()
        );
        assert_eq!(hex::encode(serialized), "0200010105");

        let item = Element::new_item(hex::decode("abcdef").expect("expected to decode"));
        let serialized = item
            .serialize(grove_version)
            .expect("expected to serialize");
        assert_eq!(serialized.len(), 6);
        assert_eq!(
            serialized.len(),
            item.serialized_size(grove_version).unwrap()
        );
        // The item is variable length 3 bytes, so it's enum 2 then 32 bytes of zeroes
        assert_eq!(hex::encode(serialized), "0003abcdef00");

        assert_eq!(hex::encode(5.encode_var_vec()), "0a");

        let item = Element::new_sum_item(5);
        let serialized = item
            .serialize(grove_version)
            .expect("expected to serialize");
        assert_eq!(serialized.len(), 3);
        assert_eq!(
            serialized.len(),
            item.serialized_size(grove_version).unwrap()
        );
        // The item is variable length 3 bytes, so it's enum 2 then 32 bytes of zeroes
        assert_eq!(hex::encode(serialized), "030a00");

        let item = Element::new_item_with_sum_item("abc".as_bytes().to_vec(), 7);
        let serialized = item
            .serialize(grove_version)
            .expect("expected to serialize ItemWithSumItem");
        assert_eq!(
            Element::deserialize(&serialized, grove_version)
                .expect("should deserialize ItemWithSumItem"),
            item
        );

        let item = Element::new_item_with_sum_item_with_flags(
            hex::decode("abcd").expect("expected to decode"),
            -3,
            Some(vec![9, 8, 7]),
        );
        let serialized = item
            .serialize(grove_version)
            .expect("expected to serialize ItemWithSumItem");
        assert_eq!(
            Element::deserialize(&serialized, grove_version)
                .expect("should deserialize ItemWithSumItem"),
            item
        );

        let item = Element::new_item_with_flags(
            hex::decode("abcdef").expect("expected to decode"),
            Some(vec![1]),
        );
        let serialized = item
            .serialize(grove_version)
            .expect("expected to serialize");
        assert_eq!(serialized.len(), 8);
        assert_eq!(
            serialized.len(),
            item.serialized_size(grove_version).unwrap()
        );
        assert_eq!(hex::encode(serialized), "0003abcdef010101");

        let reference = Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
            vec![0],
            hex::decode("abcd").expect("expected to decode"),
            vec![5],
        ]));
        let serialized = reference
            .serialize(grove_version)
            .expect("expected to serialize");
        assert_eq!(serialized.len(), 12);
        assert_eq!(
            serialized.len(),
            reference.serialized_size(grove_version).unwrap()
        );
        // The item is variable length 2 bytes, so it's enum 1 then 1 byte for length,
        // then 1 byte for 0, then 1 byte 02 for abcd, then 1 byte '1' for 05
        assert_eq!(hex::encode(serialized), "010003010002abcd01050000");

        let reference = Element::new_reference_with_flags(
            ReferencePathType::AbsolutePathReference(vec![
                vec![0],
                hex::decode("abcd").expect("expected to decode"),
                vec![5],
            ]),
            Some(vec![1, 2, 3]),
        );
        let serialized = reference
            .serialize(grove_version)
            .expect("expected to serialize");
        assert_eq!(serialized.len(), 16);
        assert_eq!(
            serialized.len(),
            reference.serialized_size(grove_version).unwrap()
        );
        assert_eq!(hex::encode(serialized), "010003010002abcd0105000103010203");
    }
}
