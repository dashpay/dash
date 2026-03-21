//! Common tests

use grovedb_path::SubtreePath;
use grovedb_version::version::GroveVersion;

use crate::{operations::proof::util::ProvedPathKeyValues, Element, Error};

/// Compare result tuples
pub fn compare_result_tuples(
    result_set: ProvedPathKeyValues,
    expected_result_set: Vec<(Vec<u8>, Vec<u8>)>,
) {
    assert_eq!(expected_result_set.len(), result_set.len());
    for i in 0..expected_result_set.len() {
        assert_eq!(expected_result_set[i].0, result_set[i].key);
        assert_eq!(expected_result_set[i].1, result_set[i].value);
    }
}

fn deserialize_and_extract_item_bytes(raw_bytes: &[u8]) -> Result<Vec<u8>, Error> {
    let elem = Element::deserialize(raw_bytes, GroveVersion::latest())?;
    match elem {
        Element::Item(item, _) => Ok(item),
        _ => Err(Error::CorruptedPath("expected only item type".to_string())),
    }
}

/// Compare result sets
pub fn compare_result_sets(elements: &Vec<Vec<u8>>, result_set: &ProvedPathKeyValues) {
    for i in 0..elements.len() {
        assert_eq!(
            deserialize_and_extract_item_bytes(&result_set[i].value).unwrap(),
            elements[i]
        )
    }
}

pub(crate) const EMPTY_PATH: SubtreePath<'static, [u8; 0]> = SubtreePath::empty();
