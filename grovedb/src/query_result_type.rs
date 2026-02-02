//! Determines the query result form

use std::{
    collections::{BTreeMap, HashMap},
    fmt,
    vec::IntoIter,
};

pub use grovedb_merk::proofs::query::{Key, Path, PathKey};
use grovedb_version::{version::GroveVersion, TryFromVersioned};

use crate::{
    operations::proof::util::{
        hex_to_ascii, path_hex_to_ascii, ProvedPathKeyOptionalValue, ProvedPathKeyValue,
    },
    Element, Error,
};

#[derive(Copy, Clone)]
/// Query result type
pub enum QueryResultType {
    /// Query element result type
    QueryElementResultType,
    /// Query key element pair result type
    QueryKeyElementPairResultType,
    /// Query path key element trio result type
    QueryPathKeyElementTrioResultType,
}

impl fmt::Display for QueryResultType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            QueryResultType::QueryElementResultType => write!(f, "QueryElementResultType"),
            QueryResultType::QueryKeyElementPairResultType => {
                write!(f, "QueryKeyElementPairResultType")
            }
            QueryResultType::QueryPathKeyElementTrioResultType => {
                write!(f, "QueryPathKeyElementTrioResultType")
            }
        }
    }
}

/// Query result elements
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct QueryResultElements {
    /// Elements
    pub elements: Vec<QueryResultElement>,
}

impl fmt::Display for QueryResultElements {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "QueryResultElements {{")?;
        for (index, element) in self.elements.iter().enumerate() {
            writeln!(f, "  {}: {}", index, element)?;
        }
        write!(f, "}}")
    }
}

#[derive(Debug, Clone)]
pub enum BTreeMapLevelResultOrItem {
    BTreeMapLevelResult(BTreeMapLevelResult),
    ResultItem(Element),
}

/// BTreeMap level result
#[derive(Debug, Clone)]
pub struct BTreeMapLevelResult {
    pub key_values: BTreeMap<Key, BTreeMapLevelResultOrItem>,
}

impl fmt::Display for BTreeMapLevelResultOrItem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BTreeMapLevelResultOrItem::BTreeMapLevelResult(result) => {
                write!(f, "{}", result)
            }
            BTreeMapLevelResultOrItem::ResultItem(element) => {
                write!(f, "{}", element)
            }
        }
    }
}

impl fmt::Display for BTreeMapLevelResult {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "BTreeMapLevelResult {{")?;
        self.fmt_inner(f, 1)?;
        write!(f, "}}")
    }
}

impl BTreeMapLevelResult {
    fn fmt_inner(&self, f: &mut fmt::Formatter<'_>, indent: usize) -> fmt::Result {
        for (key, value) in &self.key_values {
            write!(f, "{:indent$}", "", indent = indent * 2)?;
            write!(f, "{}: ", hex_to_ascii(key))?;
            match value {
                BTreeMapLevelResultOrItem::BTreeMapLevelResult(result) => {
                    writeln!(f, "BTreeMapLevelResult {{")?;
                    result.fmt_inner(f, indent + 1)?;
                    write!(f, "{:indent$}}}", "", indent = indent * 2)?;
                }
                BTreeMapLevelResultOrItem::ResultItem(element) => {
                    write!(f, "{}", element)?;
                }
            }
            writeln!(f)?;
        }
        Ok(())
    }
}

impl BTreeMapLevelResult {
    pub fn len_of_values_at_path(&self, path: &[&[u8]]) -> u16 {
        let mut current = self;

        // Traverse the path
        for segment in path {
            match current.key_values.get(*segment) {
                Some(BTreeMapLevelResultOrItem::BTreeMapLevelResult(next_level)) => {
                    current = next_level;
                }
                Some(BTreeMapLevelResultOrItem::ResultItem(_)) => {
                    // We've reached a ResultItem before the end of the path
                    return 0;
                }
                None => {
                    // Path not found
                    return 0;
                }
            }
        }

        current.key_values.len() as u16
    }
}

impl QueryResultElements {
    /// New
    pub fn new() -> Self {
        QueryResultElements { elements: vec![] }
    }

    /// From elements
    pub fn from_elements(elements: Vec<QueryResultElement>) -> Self {
        QueryResultElements { elements }
    }

    /// Length
    pub fn len(&self) -> usize {
        self.elements.len()
    }

    /// Is empty?
    pub fn is_empty(&self) -> bool {
        self.elements.is_empty()
    }

    /// Into iterator
    pub fn into_iterator(self) -> IntoIter<QueryResultElement> {
        self.elements.into_iter()
    }

    /// To elements
    pub fn to_elements(self) -> Vec<Element> {
        self.elements
            .into_iter()
            .map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(element) => element,
                QueryResultElement::KeyElementPairResultItem(element_key_pair) => {
                    element_key_pair.1
                }
                QueryResultElement::PathKeyElementTrioResultItem(path_key_element_trio) => {
                    path_key_element_trio.2
                }
            })
            .collect()
    }

    /// To key elements
    pub fn to_key_elements(self) -> Vec<KeyElementPair> {
        self.elements
            .into_iter()
            .filter_map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(_) => None,
                QueryResultElement::KeyElementPairResultItem(key_element_pair) => {
                    Some(key_element_pair)
                }
                QueryResultElement::PathKeyElementTrioResultItem(path_key_element_trio) => {
                    Some((path_key_element_trio.1, path_key_element_trio.2))
                }
            })
            .collect()
    }

    /// To keys
    pub fn to_keys(self) -> Vec<Key> {
        self.elements
            .into_iter()
            .filter_map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(_) => None,
                QueryResultElement::KeyElementPairResultItem(key_element_pair) => {
                    Some(key_element_pair.0)
                }
                QueryResultElement::PathKeyElementTrioResultItem(path_key_element_trio) => {
                    Some(path_key_element_trio.1)
                }
            })
            .collect()
    }

    /// To key elements btree map
    pub fn to_key_elements_btree_map(self) -> BTreeMap<Vec<u8>, Element> {
        self.elements
            .into_iter()
            .filter_map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(_) => None,
                QueryResultElement::KeyElementPairResultItem(key_element_pair) => {
                    Some(key_element_pair)
                }
                QueryResultElement::PathKeyElementTrioResultItem(path_key_element_trio) => {
                    Some((path_key_element_trio.1, path_key_element_trio.2))
                }
            })
            .collect()
    }

    /// To key elements hash map
    pub fn to_key_elements_hash_map(self) -> HashMap<Vec<u8>, Element> {
        self.elements
            .into_iter()
            .filter_map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(_) => None,
                QueryResultElement::KeyElementPairResultItem(key_element_pair) => {
                    Some(key_element_pair)
                }
                QueryResultElement::PathKeyElementTrioResultItem(path_key_element_trio) => {
                    Some((path_key_element_trio.1, path_key_element_trio.2))
                }
            })
            .collect()
    }

    /// To path key elements
    pub fn to_path_key_elements(self) -> Vec<PathKeyElementTrio> {
        self.elements
            .into_iter()
            .filter_map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(_) => None,
                QueryResultElement::KeyElementPairResultItem(_) => None,
                QueryResultElement::PathKeyElementTrioResultItem(path_key_element_pair) => {
                    Some(path_key_element_pair)
                }
            })
            .collect()
    }

    /// To path key elements btree map
    pub fn to_path_key_elements_btree_map(self) -> BTreeMap<PathKey, Element> {
        self.elements
            .into_iter()
            .filter_map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(_) => None,
                QueryResultElement::KeyElementPairResultItem(_) => None,
                QueryResultElement::PathKeyElementTrioResultItem((path, key, element)) => {
                    Some(((path, key), element))
                }
            })
            .collect()
    }

    /// To last path to keys btree map
    /// This is useful if for example the element is a sum item and isn't
    /// important Used in Platform Drive for getting voters for multiple
    /// contenders
    pub fn to_last_path_to_keys_btree_map(self) -> BTreeMap<Key, Vec<Key>> {
        let mut map: BTreeMap<Vec<u8>, Vec<Key>> = BTreeMap::new();

        for result_item in self.elements.into_iter() {
            if let QueryResultElement::PathKeyElementTrioResultItem((mut path, key, _)) =
                result_item
            {
                if let Some(last) = path.pop() {
                    map.entry(last).or_default().push(key);
                }
            }
        }

        map
    }

    /// To path to key, elements btree map
    pub fn to_path_to_key_elements_btree_map(self) -> BTreeMap<Path, BTreeMap<Key, Element>> {
        let mut map: BTreeMap<Path, BTreeMap<Key, Element>> = BTreeMap::new();

        for result_item in self.elements.into_iter() {
            if let QueryResultElement::PathKeyElementTrioResultItem((path, key, element)) =
                result_item
            {
                map.entry(path).or_default().insert(key, element);
            }
        }

        map
    }

    /// To last path to key, elements btree map
    pub fn to_last_path_to_key_elements_btree_map(self) -> BTreeMap<Key, BTreeMap<Key, Element>> {
        let mut map: BTreeMap<Vec<u8>, BTreeMap<Key, Element>> = BTreeMap::new();

        for result_item in self.elements.into_iter() {
            if let QueryResultElement::PathKeyElementTrioResultItem((mut path, key, element)) =
                result_item
            {
                if let Some(last) = path.pop() {
                    map.entry(last).or_default().insert(key, element);
                }
            }
        }

        map
    }

    /// To last path to elements btree map
    /// This is useful if the key is not import
    pub fn to_last_path_to_elements_btree_map(self) -> BTreeMap<Key, Vec<Element>> {
        let mut map: BTreeMap<Vec<u8>, Vec<Element>> = BTreeMap::new();

        for result_item in self.elements.into_iter() {
            if let QueryResultElement::PathKeyElementTrioResultItem((mut path, _, element)) =
                result_item
            {
                if let Some(last) = path.pop() {
                    map.entry(last).or_default().push(element);
                }
            }
        }

        map
    }

    /// To last path to elements btree map
    /// This is useful if the key is not import
    pub fn to_btree_map_level_results(self) -> BTreeMapLevelResult {
        fn insert_recursive(
            current_level: &mut BTreeMapLevelResult,
            mut path: std::vec::IntoIter<Vec<u8>>,
            key: Vec<u8>,
            element: Element,
        ) {
            if let Some(segment) = path.next() {
                let next_level = current_level.key_values.entry(segment).or_insert_with(|| {
                    BTreeMapLevelResultOrItem::BTreeMapLevelResult(BTreeMapLevelResult {
                        key_values: BTreeMap::new(),
                    })
                });

                match next_level {
                    BTreeMapLevelResultOrItem::BTreeMapLevelResult(inner) => {
                        insert_recursive(inner, path, key, element);
                    }
                    BTreeMapLevelResultOrItem::ResultItem(_) => {
                        // This shouldn't happen in a well-formed structure, but we'll handle it
                        // anyway
                        *next_level =
                            BTreeMapLevelResultOrItem::BTreeMapLevelResult(BTreeMapLevelResult {
                                key_values: BTreeMap::new(),
                            });
                        if let BTreeMapLevelResultOrItem::BTreeMapLevelResult(inner) = next_level {
                            insert_recursive(inner, path, key, element);
                        }
                    }
                }
            } else {
                current_level
                    .key_values
                    .insert(key, BTreeMapLevelResultOrItem::ResultItem(element));
            }
        }

        let mut root = BTreeMapLevelResult {
            key_values: BTreeMap::new(),
        };

        for result_item in self.elements {
            if let QueryResultElement::PathKeyElementTrioResultItem((path, key, element)) =
                result_item
            {
                insert_recursive(&mut root, path.into_iter(), key, element);
            }
        }

        root
    }

    /// To last path to keys btree map
    /// This is useful if for example the element is a sum item and isn't
    /// important Used in Platform Drive for getting voters for multiple
    /// contenders
    pub fn to_previous_of_last_path_to_keys_btree_map(self) -> BTreeMap<Key, Vec<Key>> {
        let mut map: BTreeMap<Vec<u8>, Vec<Key>> = BTreeMap::new();

        for result_item in self.elements.into_iter() {
            if let QueryResultElement::PathKeyElementTrioResultItem((mut path, key, _)) =
                result_item
            {
                if path.pop().is_some() {
                    if let Some(last) = path.pop() {
                        map.entry(last).or_default().push(key);
                    }
                }
            }
        }

        map
    }
}

impl Default for QueryResultElements {
    fn default() -> Self {
        Self::new()
    }
}

/// Query result element
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum QueryResultElement {
    /// Element result item
    ElementResultItem(Element),
    /// Key element pair result item
    KeyElementPairResultItem(KeyElementPair),
    /// Path key element trio result item
    PathKeyElementTrioResultItem(PathKeyElementTrio),
}

impl fmt::Display for QueryResultElement {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            QueryResultElement::ElementResultItem(element) => {
                write!(f, "ElementResultItem({})", element)
            }
            QueryResultElement::KeyElementPairResultItem((key, element)) => {
                write!(
                    f,
                    "KeyElementPairResultItem(key: {}, element: {})",
                    hex_to_ascii(key),
                    element
                )
            }
            QueryResultElement::PathKeyElementTrioResultItem((path, key, element)) => {
                write!(
                    f,
                    "PathKeyElementTrioResultItem(path: {}, key: {}, element: {})",
                    path_hex_to_ascii(path),
                    hex_to_ascii(key),
                    element
                )
            }
        }
    }
}

#[cfg(feature = "minimal")]
impl QueryResultElement {
    /// Map element
    pub fn map_element(
        self,
        map_function: impl FnOnce(Element) -> Result<Element, Error>,
    ) -> Result<Self, Error> {
        Ok(match self {
            QueryResultElement::ElementResultItem(element) => {
                QueryResultElement::ElementResultItem(map_function(element)?)
            }
            QueryResultElement::KeyElementPairResultItem((key, element)) => {
                QueryResultElement::KeyElementPairResultItem((key, map_function(element)?))
            }
            QueryResultElement::PathKeyElementTrioResultItem((path, key, element)) => {
                QueryResultElement::PathKeyElementTrioResultItem((
                    path,
                    key,
                    map_function(element)?,
                ))
            }
        })
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
/// Type alias for key-element common pattern.
pub type KeyElementPair = (Key, Element);

#[cfg(any(feature = "minimal", feature = "verify"))]
/// Type alias for key optional_element common pattern.
pub type KeyOptionalElementPair = (Key, Option<Element>);

#[cfg(any(feature = "minimal", feature = "verify"))]
/// Type alias for path-key-element common pattern.
pub type PathKeyElementTrio = (Path, Key, Element);

#[cfg(any(feature = "minimal", feature = "verify"))]
/// Type alias for path - key - optional_element common pattern.
pub type PathKeyOptionalElementTrio = (Path, Key, Option<Element>);

#[cfg(any(feature = "minimal", feature = "verify"))]
impl TryFromVersioned<ProvedPathKeyValue> for PathKeyOptionalElementTrio {
    type Error = Error;

    fn try_from_versioned(
        proved_path_key_value: ProvedPathKeyValue,
        grove_version: &GroveVersion,
    ) -> Result<Self, Self::Error> {
        let element = Element::deserialize(proved_path_key_value.value.as_slice(), grove_version)?;
        Ok((
            proved_path_key_value.path,
            proved_path_key_value.key,
            Some(element),
        ))
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl TryFromVersioned<ProvedPathKeyOptionalValue> for PathKeyOptionalElementTrio {
    type Error = Error;

    fn try_from_versioned(
        proved_path_key_value: ProvedPathKeyOptionalValue,
        grove_version: &GroveVersion,
    ) -> Result<Self, Self::Error> {
        let element = proved_path_key_value
            .value
            .map(|e| Element::deserialize(e.as_slice(), grove_version))
            .transpose()?;
        Ok((
            proved_path_key_value.path,
            proved_path_key_value.key,
            element,
        ))
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod tests {
    use grovedb_version::{version::GroveVersion, TryIntoVersioned};

    use crate::{
        operations::proof::util::ProvedPathKeyValue, query_result_type::PathKeyOptionalElementTrio,
        Element,
    };

    #[test]
    fn test_single_proved_path_key_value_to_path_key_optional_element() {
        let grove_version = GroveVersion::latest();
        let path = vec![b"1".to_vec(), b"2".to_vec()];
        let proved_path_key_value = ProvedPathKeyValue {
            path: path.clone(),
            key: b"a".to_vec(),
            value: vec![0, 1, 4, 0],
            proof: [0; 32],
        };
        let path_key_element_trio: PathKeyOptionalElementTrio = proved_path_key_value
            .try_into_versioned(grove_version)
            .expect("should convert to path key optional element trio");
        assert_eq!(
            path_key_element_trio,
            (path, b"a".to_vec(), Some(Element::new_item(vec![4])))
        );
    }
}
