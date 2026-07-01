// MIT LICENSE
//
// Copyright (c) 2023 Dash Core Group
//
// Permission is hereby granted, free of charge, to any
// person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the
// Software without restriction, including without
// limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice
// shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

//! Difinitions of versatile type representing a path to a subtree that can own
//! certain path segments.

use std::hash::{Hash, Hasher};

use crate::{
    subtree_path::SubtreePathInner,
    util::{CompactBytes, CowLike},
    SubtreePath, SubtreePathIter,
};

/// Path to a GroveDB's subtree.
#[derive(Debug)]
pub struct SubtreePathBuilder<'b, B> {
    /// Derivation starting point.
    pub(crate) base: SubtreePath<'b, B>,
    /// Path information relative to [base](Self::base).
    pub(crate) relative: SubtreePathRelative<'b>,
}

impl<B> Clone for SubtreePathBuilder<'_, B> {
    fn clone(&self) -> Self {
        SubtreePathBuilder {
            base: self.base.clone(),
            relative: self.relative.clone(),
        }
    }
}

/// Hash order is the same as iteration order: from most deep path segment up to
/// root.
impl<B: AsRef<[u8]>> Hash for SubtreePathBuilder<'_, B> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.relative.hash(state);
        self.base.hash(state);
    }
}

impl<'br, BL, BR> PartialEq<SubtreePathBuilder<'br, BR>> for SubtreePathBuilder<'_, BL>
where
    BL: AsRef<[u8]>,
    BR: AsRef<[u8]>,
{
    fn eq(&self, other: &SubtreePathBuilder<'br, BR>) -> bool {
        self.reverse_iter().eq(other.reverse_iter())
    }
}

impl<'br, BL, BR> PartialEq<SubtreePathBuilder<'br, BR>> for SubtreePath<'_, BL>
where
    BL: AsRef<[u8]>,
    BR: AsRef<[u8]>,
{
    fn eq(&self, other: &SubtreePathBuilder<'br, BR>) -> bool {
        self.clone().into_reverse_iter().eq(other.reverse_iter())
    }
}

impl<'br, BL, BR> PartialEq<SubtreePath<'br, BR>> for SubtreePathBuilder<'_, BL>
where
    BL: AsRef<[u8]>,
    BR: AsRef<[u8]>,
{
    fn eq(&self, other: &SubtreePath<'br, BR>) -> bool {
        self.reverse_iter().eq(other.clone().into_reverse_iter())
    }
}

impl<B: AsRef<[u8]>> Eq for SubtreePathBuilder<'_, B> {}

impl<'s, 'b, B> From<&'s SubtreePath<'b, B>> for SubtreePathBuilder<'b, B> {
    fn from(value: &'s SubtreePath<'b, B>) -> Self {
        SubtreePathBuilder {
            base: value.clone(),
            relative: SubtreePathRelative::Empty,
        }
    }
}

/// Derived subtree path on top of base path.
#[derive(Debug, Clone)]
pub(crate) enum SubtreePathRelative<'r> {
    /// Equivalent to the base path.
    Empty,
    /// Added one child segment.
    Single(CowLike<'r>),
    /// Derivation with multiple owned path segments at once
    Multi(CompactBytes),
}

impl SubtreePathRelative<'_> {
    pub fn len(&self) -> usize {
        match self {
            SubtreePathRelative::Empty => 0,
            SubtreePathRelative::Single(_) => 1,
            SubtreePathRelative::Multi(cb) => cb.len(),
        }
    }

    pub fn is_empty(&self) -> bool {
        matches!(self, SubtreePathRelative::Empty)
    }
}

impl Hash for SubtreePathRelative<'_> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        match self {
            SubtreePathRelative::Empty => {}
            SubtreePathRelative::Single(segment) => segment.hash(state),
            SubtreePathRelative::Multi(bytes) => {
                bytes.reverse_iter().for_each(|segment| segment.hash(state))
            }
        }
    }
}

impl SubtreePathBuilder<'static, [u8; 0]> {
    /// Creates empty subtree path
    pub fn new() -> Self {
        SubtreePathBuilder {
            base: [].as_ref().into(),
            relative: SubtreePathRelative::Empty,
        }
    }
}

impl Default for SubtreePathBuilder<'static, [u8; 0]> {
    fn default() -> Self {
        Self::new()
    }
}

impl<B> SubtreePathBuilder<'_, B> {
    /// Makes an owned `SubtreePathBuilder` out of iterator.
    pub fn owned_from_iter<S: AsRef<[u8]>>(iter: impl IntoIterator<Item = S>) -> Self {
        let bytes = iter.into_iter().fold(CompactBytes::new(), |mut bytes, s| {
            bytes.add_segment(s.as_ref());
            bytes
        });

        SubtreePathBuilder {
            base: SubtreePath {
                ref_variant: SubtreePathInner::Slice(&[]),
            },
            relative: SubtreePathRelative::Multi(bytes),
        }
    }

    /// Create an owned version of `SubtreePathBuilder` from `SubtreePath`.
    pub fn owned_from_path<S: AsRef<[u8]>>(path: SubtreePath<S>) -> Self {
        Self::owned_from_iter(path.to_vec())
    }
}

impl<B> SubtreePathBuilder<'_, B> {
    /// Returns the length of the subtree path.
    pub fn len(&self) -> usize {
        self.base.len() + self.relative.len()
    }

    /// Returns whether the path is empty (the root tree).
    pub fn is_empty(&self) -> bool {
        self.base.is_empty() && self.relative.is_empty()
    }

    /// Adds path segment in place.
    pub fn push_segment(&mut self, segment: &[u8]) {
        match &mut self.relative {
            SubtreePathRelative::Empty => {
                let mut bytes = CompactBytes::new();
                bytes.add_segment(segment);
                self.relative = SubtreePathRelative::Multi(bytes);
            }
            SubtreePathRelative::Single(old_segment) => {
                let mut bytes = CompactBytes::new();
                bytes.add_segment(old_segment);
                bytes.add_segment(segment);
                self.relative = SubtreePathRelative::Multi(bytes);
            }
            SubtreePathRelative::Multi(bytes) => bytes.add_segment(segment),
        }
    }
}

impl<'b, B: AsRef<[u8]>> SubtreePathBuilder<'b, B> {
    /// Get a derived path that will use another subtree path (or reuse the base
    /// slice) as it's base, then could be edited in place.
    pub fn derive_owned(&'b self) -> SubtreePathBuilder<'b, B> {
        match self.relative {
            // If this derived path makes no difference, derive from base
            SubtreePathRelative::Empty => self.base.derive_owned(),
            // Otherwise a new derived subtree path must point to this one as it's base
            _ => SubtreePathBuilder {
                base: SubtreePathInner::SubtreePath(self).into(),
                relative: SubtreePathRelative::Empty,
            },
        }
    }

    /// Get a derived path for a parent and a chopped segment. Returned
    /// [SubtreePath] will be linked to this [SubtreePath] because it might
    /// contain owned data and it has to outlive [SubtreePath].
    pub fn derive_parent(&self) -> Option<(SubtreePath<'_, B>, &[u8])> {
        match &self.relative {
            SubtreePathRelative::Empty => self.base.derive_parent(),
            SubtreePathRelative::Single(relative) => Some((self.base.clone(), relative.as_ref())),
            SubtreePathRelative::Multi(_) => {
                let mut iter = self.reverse_iter();
                iter.next()
                    .map(|segment| (SubtreePathInner::SubtreePathIter(iter).into(), segment))
            }
        }
    }

    /// Get a derived path for a parent and a chopped segment. The lifetime of
    /// returned path is constrained solely by the original slice that this
    /// whole path hierarchy is based upon, and the point of derivation has
    /// no effect on it.
    pub fn derive_parent_owned(&self) -> Option<(SubtreePathBuilder<'b, B>, Vec<u8>)> {
        match &self.relative {
            SubtreePathRelative::Empty => self
                .base
                .derive_parent()
                .map(|(path, key)| (path.derive_owned(), key.to_vec())),
            SubtreePathRelative::Single(relative) => {
                Some((self.base.derive_owned(), relative.to_vec()))
            }
            SubtreePathRelative::Multi(bytes) => {
                let mut new_bytes = bytes.clone();
                if let Some(key) = new_bytes.pop_segment() {
                    Some((
                        SubtreePathBuilder {
                            base: self.base.clone(),
                            relative: SubtreePathRelative::Multi(new_bytes),
                        },
                        key,
                    ))
                } else {
                    self.base
                        .derive_parent()
                        .map(|(path, key)| (path.derive_owned(), key.to_vec()))
                }
            }
        }
    }

    /// Get a derived path with a child path segment added.
    pub fn derive_owned_with_child<'s, S>(&'b self, segment: S) -> SubtreePathBuilder<'b, B>
    where
        S: Into<CowLike<'s>>,
        's: 'b,
    {
        SubtreePathBuilder {
            base: SubtreePathInner::SubtreePath(self).into(),
            relative: SubtreePathRelative::Single(segment.into()),
        }
    }

    /// Returns an iterator for the subtree path by path segments.
    pub fn reverse_iter(&'b self) -> SubtreePathIter<'b, B> {
        match &self.relative {
            SubtreePathRelative::Empty => self.base.clone().into_reverse_iter(),
            SubtreePathRelative::Single(item) => {
                SubtreePathIter::new_with_next(item.as_ref(), &self.base)
            }
            SubtreePathRelative::Multi(bytes) => {
                SubtreePathIter::new_with_next(bytes.reverse_iter(), &self.base)
            }
        }
    }

    /// Collect path as a vector of vectors, but this actually negates all the
    /// benefits of this library.
    pub fn to_vec(&self) -> Vec<Vec<u8>> {
        let mut result = Vec::new();

        // Because of the nature of this library, the vector will be built
        // from it's end
        match &self.relative {
            SubtreePathRelative::Empty => {}
            SubtreePathRelative::Single(s) => result.push(s.to_vec()),
            SubtreePathRelative::Multi(bytes) => {
                bytes.reverse_iter().for_each(|s| result.push(s.to_vec()))
            }
        }

        match &self.base.ref_variant {
            SubtreePathInner::Slice(slice) => slice
                .iter()
                .rev()
                .for_each(|x| result.push(x.as_ref().to_vec())),
            SubtreePathInner::SubtreePath(path) => {
                path.reverse_iter().for_each(|x| result.push(x.to_vec()))
            }
            SubtreePathInner::SubtreePathIter(iter) => {
                iter.clone().for_each(|x| result.push(x.as_ref().to_vec()))
            }
        };

        result.reverse();
        result
    }

    /// Retuns `true` if the subtree path is empty, so it points to the root
    /// tree.
    pub fn is_root(&self) -> bool {
        match self {
            Self {
                base,
                relative: SubtreePathRelative::Empty,
            } => base.is_root(),
            _ => false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn to_vec() {
        let base: SubtreePath<_> = (&[b"one" as &[u8], b"two", b"three"]).into();
        let mut builder = base.derive_owned_with_child(b"four");
        builder.push_segment(b"five");
        builder.push_segment(b"six");
        builder.push_segment(b"seven");

        let as_vec = builder.to_vec();
        let reference_vec = vec![
            b"one".to_vec(),
            b"two".to_vec(),
            b"three".to_vec(),
            b"four".to_vec(),
            b"five".to_vec(),
            b"six".to_vec(),
            b"seven".to_vec(),
        ];

        assert_eq!(as_vec, reference_vec);
        assert_eq!(builder.len(), reference_vec.len());
    }
}
