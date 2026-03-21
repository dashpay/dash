//! Space efficient methods for referencing other elements in GroveDB
pub mod util;
#[cfg(feature = "visualize")]
mod visualize;

use std::{fmt, iter};

use bincode::{Decode, Encode};
use grovedb_path::{SubtreePath, SubtreePathBuilder};
use integer_encoding::VarInt;

use crate::error::ElementError;

#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(not(feature = "visualize"), derive(Debug))]
/// Reference path variants
#[derive(Hash, Eq, PartialEq, Encode, Decode, Clone)]
pub enum ReferencePathType {
    /// Holds the absolute path to the element the reference points to
    AbsolutePathReference(Vec<Vec<u8>>),

    /// This takes the first n elements from the current path and appends a new
    /// path to the subpath. If current path is [a, b, c, d] and we take the
    /// first 2 elements, subpath = [a, b] we can then append some other
    /// path [p, q] result = [a, b, p, q]
    UpstreamRootHeightReference(u8, Vec<Vec<u8>>),

    /// This is very similar to the UpstreamRootHeightReference, however
    /// it appends to the absolute path when resolving the parent of the
    /// reference. If the reference is stored at 15/9/80/7 then 80 will be
    /// appended to what we are referring to. For example if we have the
    /// reference at [a, b, c, d, e, f] (e is the parent path here) and we
    /// have in the UpstreamRootHeightWithParentPathAdditionReference the
    /// height set to 2 and the addon path set to [x, y], we would get as a
    /// result [a, b, x, y, e]
    UpstreamRootHeightWithParentPathAdditionReference(u8, Vec<Vec<u8>>),

    /// This discards the last n elements from the current path and appends a
    /// new path to the subpath. If current path is [a, b, c, d] and we
    /// discard the last element, subpath = [a, b, c] we can then append
    /// some other path [p, q] result = [a, b, c, p, q]
    UpstreamFromElementHeightReference(u8, Vec<Vec<u8>>),

    /// This swaps the immediate parent of the stored path with a provided key,
    /// retaining the key value. e.g. current path = [a, b, m, d] you can use
    /// the cousin reference to swap m with c to get [a, b, c, d]
    CousinReference(Vec<u8>),

    /// This swaps the immediate parent of the stored path with a path,
    /// retaining the key value. e.g. current path = [a, b, c, d] you can use
    /// the removed cousin reference to swap c with m and n to get [a, b, m, n,
    /// d]
    RemovedCousinReference(Vec<Vec<u8>>),

    /// This swaps the key with a new value, you use this to point to an element
    /// in the same tree.
    SiblingReference(Vec<u8>),
}

impl ReferencePathType {
    /// Get an inverted reference
    pub fn invert<B: AsRef<[u8]>>(&self, path: SubtreePath<B>, key: &[u8]) -> Option<Self> {
        Some(match self {
            // Absolute path shall point to a fully qualified path of the reference's origin
            ReferencePathType::AbsolutePathReference(_) => {
                let mut qualified_path = path.to_vec();
                qualified_path.push(key.to_vec());
                ReferencePathType::AbsolutePathReference(qualified_path)
            }
            // Since both reference origin and path share N first segments, the backward reference
            // can do the same, key we shall persist for a qualified path as the output
            ReferencePathType::UpstreamRootHeightReference(n, _) => {
                let relative_path: Vec<_> = path
                    .to_vec()
                    .into_iter()
                    .skip(*n as usize)
                    .chain(iter::once(key.to_vec()))
                    .collect();
                ReferencePathType::UpstreamRootHeightReference(*n, relative_path)
            }
            // Since it uses some parent information it gets complicated, so falling back to the
            // previous type of reference
            ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(n, _) => {
                let relative_path: Vec<_> = path
                    .to_vec()
                    .into_iter()
                    .skip(*n as usize)
                    .chain(iter::once(key.to_vec()))
                    .collect();
                ReferencePathType::UpstreamRootHeightReference(*n, relative_path)
            }
            // Discarding N latest segments is relative to the previously appended path, so it would
            // be easier to discard appended paths both ways and have a shared prefix.
            ReferencePathType::UpstreamFromElementHeightReference(n, append_path) => {
                if append_path.len() > u8::MAX as usize {
                    return None;
                }
                let mut relative_path: Vec<Vec<u8>> = path
                    .into_reverse_iter()
                    .take(*n as usize)
                    .map(|x| x.to_vec())
                    .collect();
                relative_path.reverse();
                relative_path.push(key.to_vec());
                ReferencePathType::UpstreamFromElementHeightReference(
                    (append_path.len() as u8).saturating_sub(1),
                    relative_path,
                )
            }
            // Cousin is relative to cousin, key will remain the same
            ReferencePathType::CousinReference(_) => ReferencePathType::CousinReference(
                path.into_reverse_iter().next().map(|x| x.to_vec())?,
            ),
            // Here since any number of segments could've been added we need to resort to a more
            // specific option
            ReferencePathType::RemovedCousinReference(append_path) => {
                if append_path.len() > u8::MAX as usize {
                    return None;
                }
                let mut relative_path =
                    vec![path.into_reverse_iter().next().map(|x| x.to_vec())?];
                relative_path.push(key.to_vec());
                ReferencePathType::UpstreamFromElementHeightReference(
                    append_path.len() as u8,
                    relative_path,
                )
            }
            // The closest way back would be just to use the key
            ReferencePathType::SiblingReference(_) => {
                ReferencePathType::SiblingReference(key.to_vec())
            }
        })
    }
}

// Helper function to display paths
fn display_path(path: &[Vec<u8>]) -> String {
    path.iter()
        .map(|bytes| {
            let mut hx = hex::encode(bytes);
            if let Ok(s) = String::from_utf8(bytes.clone()) {
                if s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
                    hx.push('(');
                    hx.push_str(&s);
                    hx.push(')');
                }
            }

            hx
        })
        .collect::<Vec<String>>()
        .join("/")
}

impl fmt::Display for ReferencePathType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ReferencePathType::AbsolutePathReference(path) => {
                write!(f, "AbsolutePathReference({})", display_path(path))
            }
            ReferencePathType::UpstreamRootHeightReference(height, path) => {
                write!(
                    f,
                    "UpstreamRootHeightReference({}, {})",
                    height,
                    display_path(path)
                )
            }
            ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(height, path) => {
                write!(
                    f,
                    "UpstreamRootHeightWithParentPathAdditionReference({}, {})",
                    height,
                    display_path(path)
                )
            }
            ReferencePathType::UpstreamFromElementHeightReference(height, path) => {
                write!(
                    f,
                    "UpstreamFromElementHeightReference({}, {})",
                    height,
                    display_path(path)
                )
            }
            ReferencePathType::CousinReference(key) => {
                write!(f, "CousinReference({})", hex::encode(key))
            }
            ReferencePathType::RemovedCousinReference(path) => {
                write!(f, "RemovedCousinReference({})", display_path(path))
            }
            ReferencePathType::SiblingReference(key) => {
                write!(f, "SiblingReference({})", hex::encode(key))
            }
        }
    }
}

impl ReferencePathType {
    /// Given the reference path type and the current qualified path (path+key),
    /// this computes the absolute path of the item the reference is pointing
    /// to.
    pub fn absolute_path_using_current_qualified_path<B: AsRef<[u8]>>(
        self,
        current_qualified_path: &[B],
    ) -> Result<Vec<Vec<u8>>, ElementError> {
        path_from_reference_qualified_path_type(self, current_qualified_path)
    }

    /// Given the reference path type, the current path and the terminal key,
    /// this computes the absolute path of the item the reference is
    /// pointing to.
    pub fn absolute_path<B: AsRef<[u8]>>(
        self,
        current_path: &[B],
        current_key: Option<&[u8]>,
    ) -> Result<Vec<Vec<u8>>, ElementError> {
        path_from_reference_path_type(self, current_path, current_key)
    }

    /// TODO: deprecate the rest
    pub fn absolute_qualified_path<'b, B: AsRef<[u8]>>(
        self,
        mut current_path: SubtreePathBuilder<'b, B>,
        current_key: &[u8],
    ) -> Result<SubtreePathBuilder<'b, B>, ElementError> {
        match self {
            ReferencePathType::AbsolutePathReference(path) => {
                Ok(SubtreePathBuilder::owned_from_iter(path))
            }

            ReferencePathType::UpstreamRootHeightReference(no_of_elements_to_keep, append_path) => {
                let len = current_path.len();
                if no_of_elements_to_keep as usize > len {
                    return Err(ElementError::InvalidInput(
                        "reference stored path cannot satisfy reference constraints",
                    ));
                }
                let n_to_remove = len - no_of_elements_to_keep as usize;

                let referenced_path = (0..n_to_remove).fold(current_path, |p, _| {
                    p.derive_parent_owned()
                        .expect("lengths were checked above")
                        .0
                });
                let referenced_path = append_path.into_iter().fold(referenced_path, |mut p, s| {
                    p.push_segment(&s);
                    p
                });

                Ok(referenced_path)
            }

            ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
                no_of_elements_to_keep,
                append_path,
            ) => {
                let len = current_path.len();
                if no_of_elements_to_keep as usize > len || len < 1 {
                    return Err(ElementError::InvalidInput(
                        "reference stored path cannot satisfy reference constraints",
                    ));
                }

                let parent_key = current_path
                    .reverse_iter()
                    .next()
                    .expect("lengths were checked above")
                    .to_vec();

                let n_to_remove = len - no_of_elements_to_keep as usize;

                let referenced_path = (0..n_to_remove).fold(current_path, |p, _| {
                    p.derive_parent_owned()
                        .expect("lengths were checked above")
                        .0
                });
                let mut referenced_path =
                    append_path.into_iter().fold(referenced_path, |mut p, s| {
                        p.push_segment(&s);
                        p
                    });
                referenced_path.push_segment(&parent_key);

                Ok(referenced_path)
            }

            // Discard the last n elements from current path, append new path to subpath
            ReferencePathType::UpstreamFromElementHeightReference(
                no_of_elements_to_discard_from_end,
                append_path,
            ) => {
                let mut referenced_path = current_path;
                for _ in 0..no_of_elements_to_discard_from_end {
                    if let Some((path, _)) = referenced_path.derive_parent_owned() {
                        referenced_path = path;
                    } else {
                        return Err(ElementError::InvalidInput(
                            "reference stored path cannot satisfy reference constraints",
                        ));
                    }
                }

                let referenced_path = append_path.into_iter().fold(referenced_path, |mut p, s| {
                    p.push_segment(&s);
                    p
                });

                Ok(referenced_path)
            }

            ReferencePathType::CousinReference(cousin_key) => {
                let Some((mut referred_path, _)) = current_path.derive_parent_owned() else {
                    return Err(ElementError::InvalidInput(
                        "reference stored path cannot satisfy reference constraints",
                    ));
                };

                referred_path.push_segment(&cousin_key);
                referred_path.push_segment(current_key);

                Ok(referred_path)
            }

            ReferencePathType::RemovedCousinReference(cousin_path) => {
                let Some((mut referred_path, _)) = current_path.derive_parent_owned() else {
                    return Err(ElementError::InvalidInput(
                        "reference stored path cannot satisfy reference constraints",
                    ));
                };

                cousin_path
                    .into_iter()
                    .for_each(|s| referred_path.push_segment(&s));
                referred_path.push_segment(current_key);

                Ok(referred_path)
            }

            ReferencePathType::SiblingReference(sibling_key) => {
                current_path.push_segment(&sibling_key);
                Ok(current_path)
            }
        }
    }
}

/// Given the reference path type and the current qualified path (path+key),
/// this computes the absolute path of the item the reference is pointing to.
pub fn path_from_reference_qualified_path_type<B: AsRef<[u8]>>(
    reference_path_type: ReferencePathType,
    current_qualified_path: &[B],
) -> Result<Vec<Vec<u8>>, ElementError> {
    match current_qualified_path.split_last() {
        None => Err(ElementError::CorruptedPath(
            "qualified path should always have an element".to_string(),
        )),
        Some((key, path)) => {
            path_from_reference_path_type(reference_path_type, path, Some(key.as_ref()))
        }
    }
}

/// Given the reference path type, the current path and the terminal key, this
/// computes the absolute path of the item the reference is pointing to.
pub fn path_from_reference_path_type<B: AsRef<[u8]>>(
    reference_path_type: ReferencePathType,
    current_path: &[B],
    current_key: Option<&[u8]>,
) -> Result<Vec<Vec<u8>>, ElementError> {
    match reference_path_type {
        // No computation required, we already know the absolute path
        ReferencePathType::AbsolutePathReference(path) => Ok(path),

        // Take the first n elements from current path, append new path to subpath
        ReferencePathType::UpstreamRootHeightReference(no_of_elements_to_keep, mut path) => {
            let current_path_iter = current_path.iter();
            if usize::from(no_of_elements_to_keep) > current_path_iter.len() {
                return Err(ElementError::InvalidInput(
                    "reference stored path cannot satisfy reference constraints",
                ));
            }
            let mut subpath_as_vec = current_path_iter
                .take(no_of_elements_to_keep as usize)
                .map(|x| x.as_ref().to_vec())
                .collect::<Vec<_>>();
            subpath_as_vec.append(&mut path);
            Ok(subpath_as_vec)
        }
        ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
            no_of_elements_to_keep,
            mut path,
        ) => {
            if usize::from(no_of_elements_to_keep) > current_path.len() || current_path.is_empty() {
                return Err(ElementError::InvalidInput(
                    "reference stored path cannot satisfy reference constraints",
                ));
            }
            let last = current_path.last().unwrap().as_ref().to_vec();
            let current_path_iter = current_path.iter();
            let mut subpath_as_vec = current_path_iter
                .take(no_of_elements_to_keep as usize)
                .map(|x| x.as_ref().to_vec())
                .collect::<Vec<_>>();
            subpath_as_vec.append(&mut path);
            subpath_as_vec.push(last);
            Ok(subpath_as_vec)
        }

        // Discard the last n elements from current path, append new path to subpath
        ReferencePathType::UpstreamFromElementHeightReference(
            no_of_elements_to_discard_from_end,
            mut path,
        ) => {
            let current_path_iter = current_path.iter();
            let current_path_len = current_path_iter.len();
            if usize::from(no_of_elements_to_discard_from_end) > current_path_len {
                return Err(ElementError::InvalidInput(
                    "reference stored path cannot satisfy reference constraints",
                ));
            }

            let mut subpath_as_vec = current_path_iter
                .take(current_path_len - no_of_elements_to_discard_from_end as usize)
                .map(|x| x.as_ref().to_vec())
                .collect::<Vec<_>>();
            subpath_as_vec.append(&mut path);
            Ok(subpath_as_vec)
        }

        // Pop child, swap parent, reattach child
        ReferencePathType::CousinReference(cousin_key) => {
            let mut current_path_as_vec = current_path
                .iter()
                .map(|p| p.as_ref().to_vec())
                .collect::<Vec<Vec<u8>>>();
            if current_path_as_vec.is_empty() {
                return Err(ElementError::InvalidInput(
                    "reference stored path cannot satisfy reference constraints",
                ));
            }
            let current_key = match current_key {
                None => Err(ElementError::InvalidInput(
                    "cousin reference must supply a key",
                )),
                Some(k) => Ok(k.to_vec()),
            }?;

            current_path_as_vec.pop();
            current_path_as_vec.push(cousin_key);
            current_path_as_vec.push(current_key);
            Ok(current_path_as_vec)
        }

        // Pop child, swap parent, reattach child
        ReferencePathType::RemovedCousinReference(mut cousin_path) => {
            let mut current_path_as_vec = current_path
                .iter()
                .map(|p| p.as_ref().to_vec())
                .collect::<Vec<Vec<u8>>>();
            if current_path_as_vec.is_empty() {
                return Err(ElementError::InvalidInput(
                    "reference stored path cannot satisfy reference constraints",
                ));
            }
            let current_key = match current_key {
                None => Err(ElementError::InvalidInput(
                    "cousin reference must supply a key",
                )),
                Some(k) => Ok(k.to_vec()),
            }?;

            current_path_as_vec.pop();
            current_path_as_vec.append(&mut cousin_path);
            current_path_as_vec.push(current_key);
            Ok(current_path_as_vec)
        }

        // Pop child, attach new child
        ReferencePathType::SiblingReference(sibling_key) => {
            let mut current_path_as_vec = current_path
                .iter()
                .map(|p| p.as_ref().to_vec())
                .collect::<Vec<Vec<u8>>>();
            current_path_as_vec.push(sibling_key);
            Ok(current_path_as_vec)
        }
    }
}

impl ReferencePathType {
    /// Serialized size
    pub fn serialized_size(&self) -> usize {
        match self {
            ReferencePathType::AbsolutePathReference(path)
            | ReferencePathType::RemovedCousinReference(path) => {
                1 + path
                    .iter()
                    .map(|inner| {
                        let inner_len = inner.len();
                        inner_len + inner_len.required_space()
                    })
                    .sum::<usize>()
            }
            ReferencePathType::UpstreamRootHeightReference(_, path)
            | ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(_, path)
            | ReferencePathType::UpstreamFromElementHeightReference(_, path) => {
                1 + 1
                    + path
                        .iter()
                        .map(|inner| {
                            let inner_len = inner.len();
                            inner_len + inner_len.required_space()
                        })
                        .sum::<usize>()
            }
            ReferencePathType::CousinReference(path)
            | ReferencePathType::SiblingReference(path) => {
                1 + path.len() + path.len().required_space()
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use grovedb_path::{SubtreePath, SubtreePathBuilder};

    use crate::reference_path::{path_from_reference_path_type, ReferencePathType};

    #[test]
    fn test_upstream_root_height_reference() {
        let stored_path = vec![b"a".as_ref(), b"b".as_ref(), b"m".as_ref()];
        // selects the first 2 elements from the stored path and appends the new path.
        let ref1 =
            ReferencePathType::UpstreamRootHeightReference(2, vec![b"c".to_vec(), b"d".to_vec()]);
        let final_path = path_from_reference_path_type(ref1, &stored_path, None).unwrap();
        assert_eq!(
            final_path,
            vec![b"a".to_vec(), b"b".to_vec(), b"c".to_vec(), b"d".to_vec()]
        );
    }

    #[test]
    fn test_upstream_root_height_reference_path_lib() {
        let stored_path: SubtreePathBuilder<&[u8]> =
            SubtreePathBuilder::owned_from_iter([b"a".as_ref(), b"b".as_ref(), b"m".as_ref()]);
        // selects the first 2 elements from the stored path and appends the new path.
        let ref1 =
            ReferencePathType::UpstreamRootHeightReference(2, vec![b"c".to_vec(), b"d".to_vec()]);
        let final_path = ref1.absolute_qualified_path(stored_path, b"").unwrap();
        assert_eq!(
            final_path.to_vec(),
            vec![b"a".to_vec(), b"b".to_vec(), b"c".to_vec(), b"d".to_vec()]
        );
    }

    #[test]
    fn test_upstream_root_height_with_parent_addition_reference() {
        let stored_path = vec![b"a".as_ref(), b"b".as_ref(), b"m".as_ref()];
        // selects the first 2 elements from the stored path and appends the new path.
        let ref1 = ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
            2,
            vec![b"c".to_vec(), b"d".to_vec()],
        );
        let final_path = path_from_reference_path_type(ref1, &stored_path, None).unwrap();
        assert_eq!(
            final_path,
            vec![
                b"a".to_vec(),
                b"b".to_vec(),
                b"c".to_vec(),
                b"d".to_vec(),
                b"m".to_vec()
            ]
        );
    }

    #[test]
    fn test_upstream_root_height_with_parent_addition_reference_path_lib() {
        let stored_path: SubtreePathBuilder<&[u8]> =
            SubtreePathBuilder::owned_from_iter([b"a".as_ref(), b"b".as_ref(), b"m".as_ref()]);
        // selects the first 2 elements from the stored path and appends the new path.
        let ref1 = ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
            2,
            vec![b"c".to_vec(), b"d".to_vec()],
        );
        let final_path = ref1.absolute_qualified_path(stored_path, b"").unwrap();
        assert_eq!(
            final_path.to_vec(),
            vec![
                b"a".to_vec(),
                b"b".to_vec(),
                b"c".to_vec(),
                b"d".to_vec(),
                b"m".to_vec()
            ]
        );
    }

    #[test]
    fn test_upstream_from_element_height_reference() {
        let stored_path = vec![b"a".as_ref(), b"b".as_ref(), b"m".as_ref()];
        // discards the last element from the stored_path
        let ref1 = ReferencePathType::UpstreamFromElementHeightReference(
            1,
            vec![b"c".to_vec(), b"d".to_vec()],
        );
        let final_path = path_from_reference_path_type(ref1, &stored_path, None).unwrap();
        assert_eq!(
            final_path,
            vec![b"a".to_vec(), b"b".to_vec(), b"c".to_vec(), b"d".to_vec()]
        );
    }

    #[test]
    fn test_upstream_from_element_height_reference_path_lib() {
        let stored_path: SubtreePathBuilder<&[u8]> =
            SubtreePathBuilder::owned_from_iter([b"a".as_ref(), b"b".as_ref(), b"m".as_ref()]);
        // discards the last element from the stored_path
        let ref1 = ReferencePathType::UpstreamFromElementHeightReference(
            1,
            vec![b"c".to_vec(), b"d".to_vec()],
        );
        let final_path = ref1.absolute_qualified_path(stored_path, b"").unwrap();
        assert_eq!(
            final_path.to_vec(),
            vec![b"a".to_vec(), b"b".to_vec(), b"c".to_vec(), b"d".to_vec()]
        );
    }

    #[test]
    fn test_cousin_reference_no_key() {
        let stored_path = vec![b"a".as_ref(), b"b".as_ref(), b"m".as_ref()];
        // Replaces the immediate parent (in this case b) with the given key (c)
        let ref1 = ReferencePathType::CousinReference(b"c".to_vec());
        let final_path = path_from_reference_path_type(ref1, &stored_path, None);
        assert!(final_path.is_err());
    }

    #[test]
    fn test_cousin_reference() {
        let stored_path = vec![b"a".as_ref(), b"b".as_ref()];
        let key = b"m".as_ref();
        // Replaces the immediate parent (in this case b) with the given key (c)
        let ref1 = ReferencePathType::CousinReference(b"c".to_vec());
        let final_path = path_from_reference_path_type(ref1, &stored_path, Some(key)).unwrap();
        assert_eq!(
            final_path,
            vec![b"a".to_vec(), b"c".to_vec(), b"m".to_vec()]
        );
    }

    #[test]
    fn test_cousin_reference_path_lib() {
        let stored_path: SubtreePathBuilder<&[u8]> =
            SubtreePathBuilder::owned_from_iter([b"a".as_ref(), b"b".as_ref()]);
        let key = b"m".as_ref();
        // Replaces the immediate parent (in this case b) with the given key (c)
        let ref1 = ReferencePathType::CousinReference(b"c".to_vec());
        let final_path = ref1.absolute_qualified_path(stored_path, key).unwrap();
        assert_eq!(
            final_path.to_vec(),
            vec![b"a".to_vec(), b"c".to_vec(), b"m".to_vec()]
        );
    }

    #[test]
    fn test_removed_cousin_reference_no_key() {
        let stored_path = vec![b"a".as_ref(), b"b".as_ref(), b"m".as_ref()];
        // Replaces the immediate parent (in this case b) with the given key (c)
        let ref1 = ReferencePathType::RemovedCousinReference(vec![b"c".to_vec(), b"d".to_vec()]);
        let final_path = path_from_reference_path_type(ref1, &stored_path, None);
        assert!(final_path.is_err());
    }

    #[test]
    fn test_removed_cousin_reference() {
        let stored_path = vec![b"a".as_ref(), b"b".as_ref()];
        let key = b"m".as_ref();
        // Replaces the immediate parent (in this case b) with the given key (c)
        let ref1 = ReferencePathType::RemovedCousinReference(vec![b"c".to_vec(), b"d".to_vec()]);
        let final_path = path_from_reference_path_type(ref1, &stored_path, Some(key)).unwrap();
        assert_eq!(
            final_path,
            vec![b"a".to_vec(), b"c".to_vec(), b"d".to_vec(), b"m".to_vec()]
        );
    }

    #[test]
    fn test_removed_cousin_reference_path_lib() {
        let stored_path: SubtreePathBuilder<&[u8]> =
            SubtreePathBuilder::owned_from_iter([b"a".as_ref(), b"b".as_ref()]);
        let key = b"m".as_ref();
        // Replaces the immediate parent (in this case b) with the given key (c)
        let ref1 = ReferencePathType::RemovedCousinReference(vec![b"c".to_vec(), b"d".to_vec()]);
        let final_path = ref1.absolute_qualified_path(stored_path, key).unwrap();
        assert_eq!(
            final_path.to_vec(),
            vec![b"a".to_vec(), b"c".to_vec(), b"d".to_vec(), b"m".to_vec()]
        );
    }

    #[test]
    fn test_sibling_reference() {
        let stored_path = vec![b"a".as_ref(), b"b".as_ref()];
        let key = b"m".as_ref();
        let ref1 = ReferencePathType::SiblingReference(b"c".to_vec());
        let final_path = path_from_reference_path_type(ref1, &stored_path, Some(key)).unwrap();
        assert_eq!(
            final_path,
            vec![b"a".to_vec(), b"b".to_vec(), b"c".to_vec()]
        );
    }

    #[test]
    fn test_sibling_reference_path_lib() {
        let stored_path: SubtreePathBuilder<&[u8]> =
            SubtreePathBuilder::owned_from_iter([b"a".as_ref(), b"b".as_ref()]);
        let key = b"m".as_ref();
        let ref1 = ReferencePathType::SiblingReference(b"c".to_vec());
        let final_path = ref1.absolute_qualified_path(stored_path, key).unwrap();
        assert_eq!(
            final_path.to_vec(),
            vec![b"a".to_vec(), b"b".to_vec(), b"c".to_vec()]
        );
    }

    #[test]
    fn inverted_absolute_path() {
        let current_path: SubtreePath<_> = (&[b"a", b"b", b"c", b"d"]).into();
        let current_key = b"e";
        let current_qualified_path = {
            let mut p = current_path.to_vec();
            p.push(current_key.to_vec());
            p
        };

        let reference =
            ReferencePathType::AbsolutePathReference(vec![b"m".to_vec(), b"n".to_vec()]);

        let pointed_to_qualified_path = reference
            .clone()
            .absolute_path(&current_path.to_vec(), Some(current_key))
            .unwrap();

        let (pointed_to_key, pointed_to_path) = pointed_to_qualified_path.split_last().unwrap();

        let inverse = reference.invert(current_path.clone(), current_key).unwrap();

        assert_ne!(reference, inverse);

        assert_eq!(
            reference,
            inverse
                .invert(pointed_to_path.into(), pointed_to_key)
                .unwrap()
        );
        assert_eq!(
            inverse
                .absolute_path(pointed_to_path, Some(pointed_to_key))
                .unwrap(),
            current_qualified_path
        );
    }

    #[test]
    fn inverted_upstream_root_height() {
        let current_path: SubtreePath<_> = (&[b"a", b"b", b"c", b"d"]).into();
        let current_key = b"e";
        let current_qualified_path = {
            let mut p = current_path.to_vec();
            p.push(current_key.to_vec());
            p
        };

        let reference =
            ReferencePathType::UpstreamRootHeightReference(2, vec![b"m".to_vec(), b"n".to_vec()]);

        let pointed_to_qualified_path = reference
            .clone()
            .absolute_path(&current_path.to_vec(), None)
            .unwrap();
        let (pointed_to_key, pointed_to_path) = pointed_to_qualified_path.split_last().unwrap();

        let inverse = reference.invert(current_path.clone(), current_key).unwrap();

        assert_ne!(reference, inverse);

        assert_eq!(
            reference,
            inverse
                .invert(pointed_to_path.into(), pointed_to_key)
                .unwrap()
        );
        assert_eq!(
            inverse
                .absolute_path(pointed_to_path, Some(pointed_to_key))
                .unwrap(),
            current_qualified_path.to_vec(),
        );
    }

    #[test]
    fn inverted_upstream_root_height_with_parent_path_addition() {
        let current_path: SubtreePath<_> = (&[b"a", b"b", b"c", b"d"]).into();
        let current_key = b"e";
        let current_qualified_path = {
            let mut p = current_path.to_vec();
            p.push(current_key.to_vec());
            p
        };
        let reference = ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
            2,
            vec![b"m".to_vec(), b"n".to_vec()],
        );

        let pointed_to_qualified_path = reference
            .clone()
            .absolute_path(&current_path.to_vec(), Some(current_key))
            .unwrap();
        let (pointed_to_key, pointed_to_path) = pointed_to_qualified_path.split_last().unwrap();

        let inverse = reference.invert(current_path.clone(), current_key).unwrap();

        assert_ne!(reference, inverse);

        assert_eq!(
            inverse
                .absolute_path(pointed_to_path, Some(pointed_to_key))
                .unwrap(),
            current_qualified_path.to_vec(),
        );
    }

    #[test]
    fn inverted_upstream_from_element_height() {
        {
            let current_path: SubtreePath<_> = (&[b"a", b"b", b"c", b"d"]).into();
            let current_key = b"e";
            let current_qualified_path = {
                let mut p = current_path.to_vec();
                p.push(current_key.to_vec());
                p
            };
            let reference = ReferencePathType::UpstreamFromElementHeightReference(
                1,
                vec![b"m".to_vec(), b"n".to_vec()],
            );

            let pointed_to_qualified_path = reference
                .clone()
                .absolute_path(&current_path.to_vec(), Some(current_key))
                .unwrap();
            let (pointed_to_key, pointed_to_path) = pointed_to_qualified_path.split_last().unwrap();

            let inverse = reference.invert(current_path.clone(), current_key).unwrap();

            assert_ne!(reference, inverse);

            assert_eq!(
                reference,
                inverse
                    .invert(pointed_to_path.into(), pointed_to_key)
                    .unwrap()
            );
            assert_eq!(
                inverse
                    .absolute_path(pointed_to_path, Some(pointed_to_key))
                    .unwrap(),
                current_qualified_path.to_vec(),
            );
        }

        {
            let current_path: SubtreePath<_> = (&[b"a", b"b", b"c", b"d"]).into();
            let current_key = b"e";
            let current_qualified_path = {
                let mut p = current_path.to_vec();
                p.push(current_key.to_vec());
                p
            };
            let reference = ReferencePathType::UpstreamFromElementHeightReference(
                3,
                vec![b"m".to_vec(), b"n".to_vec()],
            );

            let pointed_to_qualified_path = reference
                .clone()
                .absolute_path(&current_path.to_vec(), Some(current_key))
                .unwrap();
            let (pointed_to_key, pointed_to_path) = pointed_to_qualified_path.split_last().unwrap();

            let inverse = reference.invert(current_path.clone(), current_key).unwrap();

            assert_ne!(reference, inverse);

            assert_eq!(
                reference,
                inverse
                    .invert(pointed_to_path.into(), pointed_to_key)
                    .unwrap()
            );
            assert_eq!(
                inverse
                    .absolute_path(pointed_to_path, Some(pointed_to_key))
                    .unwrap(),
                current_qualified_path.to_vec(),
            );
        }
    }

    #[test]
    fn inverted_cousin_reference() {
        let current_path: SubtreePath<_> = (&[b"a", b"b", b"c", b"d"]).into();
        let current_key = b"e";
        let current_qualified_path = {
            let mut p = current_path.to_vec();
            p.push(current_key.to_vec());
            p
        };
        let reference =
            ReferencePathType::RemovedCousinReference(vec![b"m".to_vec(), b"n".to_vec()]);

        let pointed_to_qualified_path = reference
            .clone()
            .absolute_path(&current_path.to_vec(), Some(current_key))
            .unwrap();
        let (pointed_to_key, pointed_to_path) = pointed_to_qualified_path.split_last().unwrap();

        let inverse = reference.invert(current_path.clone(), current_key).unwrap();

        assert_ne!(reference, inverse);
        assert_eq!(
            inverse
                .absolute_path(pointed_to_path, Some(pointed_to_key))
                .unwrap(),
            current_qualified_path
        );
    }

    #[test]
    fn inverted_sibling_reference() {
        let current_path: SubtreePath<_> = (&[b"a", b"b", b"c", b"d"]).into();
        let current_key = b"e";
        let current_qualified_path = {
            let mut p = current_path.to_vec();
            p.push(current_key.to_vec());
            p
        };
        let reference = ReferencePathType::SiblingReference(b"yeet".to_vec());

        let pointed_to_qualified_path = reference
            .clone()
            .absolute_path(&current_path.to_vec(), Some(current_key))
            .unwrap();
        let (pointed_to_key, pointed_to_path) = pointed_to_qualified_path.split_last().unwrap();

        let inverse = reference.invert(current_path.clone(), current_key).unwrap();

        assert_ne!(reference, inverse);
        assert_eq!(
            reference,
            inverse
                .invert(pointed_to_path.into(), pointed_to_key)
                .unwrap()
        );
        assert_eq!(
            inverse
                .absolute_path(pointed_to_path, Some(pointed_to_key))
                .unwrap(),
            current_qualified_path
        );
    }
}
