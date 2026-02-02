//! Query

#![allow(unstable_name_collisions)]

#[cfg(feature = "minimal")]
use std::{
    collections::{btree_map, btree_map::Iter, BTreeMap},
    ops::{Bound, RangeBounds},
};

#[cfg(feature = "minimal")]
use super::super::Node;
#[cfg(feature = "minimal")]
use crate::error::Error;

#[cfg(feature = "minimal")]
/// `MapBuilder` allows a consumer to construct a `Map` by inserting the nodes
/// contained in a proof, in key-order.
pub struct MapBuilder(Map);

#[cfg(feature = "minimal")]
impl Default for MapBuilder {
    fn default() -> Self {
        MapBuilder::new()
    }
}

#[cfg(feature = "minimal")]
impl MapBuilder {
    /// Creates a new `MapBuilder` with an empty internal `Map`.
    pub fn new() -> Self {
        Self(Map {
            entries: Default::default(),
            right_edge: true,
        })
    }

    /// Adds the node's data to the underlying `Map` (if node is type `KV`), or
    /// makes a note of non-contiguous data (if node is type `KVHash` or
    /// `Hash`).
    pub fn insert(&mut self, node: &Node) -> Result<(), Error> {
        match node {
            Node::KV(key, value)
            | Node::KVValueHash(key, value, ..)
            | Node::KVCount(key, value, _)
            | Node::KVValueHashFeatureType(key, value, ..) => {
                if let Some((prev_key, _)) = self.0.entries.last_key_value() {
                    if key <= prev_key {
                        return Err(Error::KeyOrderingError(
                            "Expected nodes to be in increasing key order",
                        ));
                    }
                }

                let value = (self.0.right_edge, value.clone());
                self.0.entries.insert(key.clone(), value);
                self.0.right_edge = true;
            }
            _ => self.0.right_edge = false,
        }

        Ok(())
    }

    /// Consumes the `MapBuilder` and returns its internal `Map`.
    pub fn build(self) -> Map {
        self.0
    }
}

#[cfg(feature = "minimal")]
/// `Map` stores data extracted from a proof (which has already been verified
/// against a known root hash), and allows a consumer to access the data by
/// looking up individual keys using the `get` method, or iterating over ranges
/// using the `range` method.
#[derive(Debug)]
pub struct Map {
    entries: BTreeMap<Vec<u8>, (bool, Vec<u8>)>,
    right_edge: bool,
}

#[cfg(feature = "minimal")]
impl Map {
    /// Gets the value for a single key, or `None` if the key was proven to not
    /// exist in the tree. If the proof does not include the data and also does
    /// not prove that the key is absent in the tree (meaning the proof is not
    /// valid), an error will be returned.
    pub fn get<'a>(&'a self, key: &'a [u8]) -> Result<Option<&'a [u8]>, Error> {
        // if key is in proof just get from entries
        if let Some((_, value)) = self.entries.get(key) {
            return Ok(Some(value.as_slice()));
        }

        // otherwise, use range which only includes exact key match to check
        // absence proof
        let entry = self
            .range((Bound::Included(key), Bound::Included(key)))
            .next()
            .transpose()?
            .map(|(_, value)| value);
        Ok(entry)
    }

    /// Iterate through all entries
    pub fn all(&self) -> Iter<'_, Vec<u8>, (bool, Vec<u8>)> {
        self.entries.iter()
    }

    /// Returns an iterator over all (key, value) entries in the requested range
    /// of keys. If during iteration we encounter a gap in the data (e.g. the
    /// proof did not include all nodes within the range), the iterator will
    /// yield an error.
    pub fn range<'a, R: RangeBounds<&'a [u8]>>(&'a self, bounds: R) -> Range<'a> {
        let start_key = bound_to_inner(bounds.start_bound()).map(|x| (*x).into());
        let bounds = bounds_to_vec(bounds);

        Range {
            map: self,
            prev_key: start_key.as_ref().cloned(),
            start_key,
            iter: self.entries.range(bounds),
        }
    }
}

#[cfg(feature = "minimal")]
/// Returns `None` for `Bound::Unbounded`, or the inner key value for
/// `Bound::Included` and `Bound::Excluded`.
fn bound_to_inner<T>(bound: Bound<T>) -> Option<T> {
    match bound {
        Bound::Unbounded => None,
        Bound::Included(key) | Bound::Excluded(key) => Some(key),
    }
}

#[cfg(feature = "minimal")]
fn bound_to_vec(bound: Bound<&&[u8]>) -> Bound<Vec<u8>> {
    match bound {
        Bound::Unbounded => Bound::Unbounded,
        Bound::Excluded(k) => Bound::Excluded(k.to_vec()),
        Bound::Included(k) => Bound::Included(k.to_vec()),
    }
}

#[cfg(feature = "minimal")]
fn bounds_to_vec<'a, R: RangeBounds<&'a [u8]>>(bounds: R) -> impl RangeBounds<Vec<u8>> {
    (
        bound_to_vec(bounds.start_bound()),
        bound_to_vec(bounds.end_bound()),
    )
}

#[cfg(feature = "minimal")]
/// An iterator over (key, value) entries as extracted from a verified proof. If
/// during iteration we encounter a gap in the data (e.g. the proof did not
/// include all nodes within the range), the iterator will yield an error.
pub struct Range<'a> {
    map: &'a Map,
    start_key: Option<Vec<u8>>,
    iter: btree_map::Range<'a, Vec<u8>, (bool, Vec<u8>)>,
    prev_key: Option<Vec<u8>>,
}

#[cfg(feature = "minimal")]
impl Range<'_> {
    /// Returns an error if the proof does not properly prove the end of the
    /// range.
    fn check_end_bound(&self) -> Result<(), Error> {
        let excluded_data = match self.prev_key {
            // unbounded end, ensure proof has not excluded data at global right
            // edge of tree
            None => !self.map.right_edge,

            // bounded end (inclusive or exclusive), ensure we had an exact
            // match or next node is contiguous
            Some(ref key) => {
                // get neighboring node to the right (if any)
                let range = (Bound::Excluded(key.to_vec()), Bound::<Vec<u8>>::Unbounded);
                let maybe_end_node = self.map.entries.range(range).next();

                match maybe_end_node {
                    // reached global right edge of tree
                    None => !self.map.right_edge,

                    // got end node, must be contiguous
                    Some((_, (contiguous, _))) => !contiguous,
                }
            }
        };

        if excluded_data {
            Err(Error::InvalidProofError(
                "Proof is missing data for query".to_string(),
            ))
        } else {
            Ok(())
        }
    }
}

#[cfg(feature = "minimal")]
impl<'a> Iterator for Range<'a> {
    type Item = Result<(&'a [u8], &'a [u8]), Error>;

    fn next(&mut self) -> Option<Self::Item> {
        let (key, (contiguous, value)) = match self.iter.next() {
            // no more items, ensure no data was excluded at end of range
            None => {
                return match self.check_end_bound() {
                    Err(err) => Some(Err(err)),
                    Ok(_) => None,
                }
            }

            // got next item, destructure
            Some((key, (contiguous, value))) => (key, (contiguous, value)),
        };

        self.prev_key = Some(key.clone());

        // don't check for contiguous nodes if we have an exact match for lower
        // bound
        let skip_exclusion_check = if let Some(ref start_key) = self.start_key {
            start_key == key
        } else {
            false
        };

        // if nodes weren't contiguous, we cannot verify that we have all values
        // in the desired range
        if !skip_exclusion_check && !*contiguous {
            return Some(Err(Error::InvalidProofError(
                "Proof is not contiguous for query".to_string(),
            )));
        }

        // passed checks, return entry
        Some(Ok((key.as_slice(), value.as_slice())))
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod tests {
    use super::*;
    use crate::HASH_LENGTH;

    #[test]
    #[should_panic(expected = "Expected nodes to be in increasing key order")]
    fn mapbuilder_insert_out_of_order() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 2], vec![])).unwrap();
    }

    #[test]
    #[should_panic(expected = "Expected nodes to be in increasing key order")]
    fn mapbuilder_insert_dupe() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![])).unwrap();
    }

    #[test]
    fn mapbuilder_insert_including_edge() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::Hash([0; HASH_LENGTH])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 4], vec![])).unwrap();

        assert!(builder.0.right_edge);
    }

    #[test]
    fn mapbuilder_insert_abridged_edge() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![])).unwrap();
        builder.insert(&Node::Hash([0; HASH_LENGTH])).unwrap();

        assert!(!builder.0.right_edge);
    }

    #[test]
    fn mapbuilder_build() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![1])).unwrap();
        builder.insert(&Node::Hash([0; HASH_LENGTH])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 4], vec![2])).unwrap();

        let map = builder.build();
        let mut entries = map.entries.iter();
        assert_eq!(entries.next(), Some((&vec![1, 2, 3], &(true, vec![1]))));
        assert_eq!(entries.next(), Some((&vec![1, 2, 4], &(false, vec![2]))));
        assert_eq!(entries.next(), None);
        assert!(map.right_edge);
    }

    #[test]
    fn map_get_included() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![1])).unwrap();
        builder.insert(&Node::Hash([0; HASH_LENGTH])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 4], vec![2])).unwrap();

        let map = builder.build();
        assert_eq!(map.get(&[1, 2, 3]).unwrap().unwrap(), vec![1],);
        assert_eq!(map.get(&[1, 2, 4]).unwrap().unwrap(), vec![2],);
    }

    #[test]
    #[should_panic(expected = "Proof is missing data for query")]
    fn map_get_missing_absence_proof() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![1])).unwrap();
        builder.insert(&Node::Hash([0; HASH_LENGTH])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 4], vec![2])).unwrap();

        let map = builder.build();
        map.get(&[1, 2, 3, 4]).unwrap();
    }

    #[test]
    fn map_get_valid_absence_proof() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![1])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 4], vec![2])).unwrap();

        let map = builder.build();
        assert!(map.get(&[1, 2, 3, 4]).unwrap().is_none());
    }

    #[test]
    #[should_panic(expected = "Proof is missing data for query")]
    fn range_abridged() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![1])).unwrap();
        builder.insert(&Node::Hash([0; HASH_LENGTH])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 4], vec![2])).unwrap();

        let map = builder.build();
        let mut range = map.range(&[1u8, 2, 3][..]..&[1u8, 2, 4][..]);
        assert_eq!(range.next().unwrap().unwrap(), (&[1, 2, 3][..], &[1][..]));
        range.next().unwrap().unwrap();
    }

    #[test]
    fn range_ok() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![1])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 4], vec![2])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 5], vec![3])).unwrap();

        let map = builder.build();
        let mut range = map.range(&[1u8, 2, 3][..]..&[1u8, 2, 5][..]);
        assert_eq!(range.next().unwrap().unwrap(), (&[1, 2, 3][..], &[1][..]));
        assert_eq!(range.next().unwrap().unwrap(), (&[1, 2, 4][..], &[2][..]));
        assert!(range.next().is_none());
    }

    #[test]
    #[should_panic(expected = "Proof is not contiguous for query")]
    fn range_lower_unbounded_map_non_contiguous() {
        let mut builder = MapBuilder::new();
        builder.insert(&Node::KV(vec![1, 2, 3], vec![1])).unwrap();
        builder.insert(&Node::Hash([1; HASH_LENGTH])).unwrap();
        builder.insert(&Node::KV(vec![1, 2, 4], vec![1])).unwrap();

        let map = builder.build();

        let mut range = map.range(..&[1u8, 2, 5][..]);
        range.next().unwrap().unwrap();
        assert_eq!(range.next().unwrap().unwrap(), (&[1][..], &[1][..]));
    }
}
