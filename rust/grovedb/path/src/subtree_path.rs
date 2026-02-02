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

//! Definitions of type representing a path to a subtree made of borrowed data.
//!
//! Opposed to [SubtreePathBuilder] which is some kind of a builder,
//! [SubtreePath] is a way to refer to path data which makes it a great
//! candidate to use as a function argument where a subtree path is expected,
//! combined with it's various `From` implementations it can cover slices, owned
//! subtree paths and other path references if use as generic [Into].

use std::{
    cmp,
    fmt::{self, Display},
    hash::{Hash, Hasher},
};

use crate::{
    subtree_path_builder::{SubtreePathBuilder, SubtreePathRelative},
    util::CowLike,
    SubtreePathIter,
};

/// Path to a GroveDB's subtree with no owned data and cheap to clone.
#[derive(Debug)]
pub struct SubtreePath<'b, B> {
    pub(crate) ref_variant: SubtreePathInner<'b, B>,
}

impl<B: AsRef<[u8]>> Display for SubtreePath<'_, B> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> std::fmt::Result {
        fn bytes_to_hex_or_ascii(bytes: &[u8]) -> String {
            // Define the set of allowed characters
            const ALLOWED_CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ\
                                  abcdefghijklmnopqrstuvwxyz\
                                  0123456789_-/\\[]@";

            // Check if all characters in hex_value are allowed
            if bytes.iter().all(|&c| ALLOWED_CHARS.contains(&c)) {
                // Try to convert to UTF-8
                String::from_utf8(bytes.to_vec())
                    .unwrap_or_else(|_| format!("0x{}", hex::encode(bytes)))
            } else {
                // Hex encode and prepend "0x"
                format!("0x{}", hex::encode(bytes))
            }
        }

        match &self.ref_variant {
            SubtreePathInner::Slice(slice) => {
                let ascii_path = slice
                    .iter()
                    .map(|e| bytes_to_hex_or_ascii(e.as_ref()))
                    .collect::<Vec<_>>()
                    .join("/");
                write!(f, "{}", ascii_path)
            }
            SubtreePathInner::SubtreePath(subtree_path) => {
                let ascii_path = subtree_path
                    .to_vec()
                    .into_iter()
                    .map(|a| bytes_to_hex_or_ascii(a.as_slice()))
                    .collect::<Vec<_>>()
                    .join("/");
                write!(f, "{}", ascii_path)
            }
            SubtreePathInner::SubtreePathIter(iter) => {
                let ascii_path = iter
                    .clone()
                    .map(bytes_to_hex_or_ascii)
                    .collect::<Vec<_>>()
                    .join("/");
                write!(f, "{}", ascii_path)
            }
        }
    }
}

/// Wrapped inner representation of subtree path ref.
#[derive(Debug)]
pub(crate) enum SubtreePathInner<'b, B> {
    /// The referred path is a slice, might a provided by user or a subslice
    /// when deriving a parent.
    Slice(&'b [B]),
    /// Links to an existing subtree path that became a derivation point.
    SubtreePath(&'b SubtreePathBuilder<'b, B>),
    /// Links to an existing subtree path with owned segments using it's
    /// iterator to support parent derivations.
    /// This may sound tricky, but `SubtreePathIter` fits there nicely because
    /// like the other variants of [SubtreePathInner] it points to some segments
    /// data, but because of parent derivations on packed path segments we need
    /// to keep track where are we, that's exactly what iterator does + holds a
    /// link to the next part of our subtree path chain.
    SubtreePathIter(SubtreePathIter<'b, B>),
}

impl<'br, BL, BR> PartialEq<SubtreePath<'br, BR>> for SubtreePath<'_, BL>
where
    BL: AsRef<[u8]>,
    BR: AsRef<[u8]>,
{
    fn eq(&self, other: &SubtreePath<'br, BR>) -> bool {
        self.clone()
            .into_reverse_iter()
            .eq(other.clone().into_reverse_iter())
    }
}

/// First and foremost, the order of subtree paths is dictated by their lengths.
/// Therefore, those subtrees closer to the root will come first. The rest it
/// can guarantee is to be free of false equality; however, seemingly unrelated
/// subtrees can come one after another if they share the same length, which was
/// (not) done for performance reasons.
impl<'br, BL, BR> PartialOrd<SubtreePath<'br, BR>> for SubtreePath<'_, BL>
where
    BL: AsRef<[u8]>,
    BR: AsRef<[u8]>,
{
    fn partial_cmp(&self, other: &SubtreePath<'br, BR>) -> Option<cmp::Ordering> {
        let iter_a = self.clone().into_reverse_iter();
        let iter_b = other.clone().into_reverse_iter();

        Some(
            iter_a
                .len()
                .cmp(&iter_b.len())
                .reverse()
                .then_with(|| iter_a.cmp(iter_b)),
        )
    }
}

impl<'br, BL, BR> PartialOrd<SubtreePathBuilder<'br, BR>> for SubtreePathBuilder<'_, BL>
where
    BL: AsRef<[u8]>,
    BR: AsRef<[u8]>,
{
    fn partial_cmp(&self, other: &SubtreePathBuilder<'br, BR>) -> Option<cmp::Ordering> {
        let iter_a = self.reverse_iter();
        let iter_b = other.reverse_iter();

        Some(
            iter_a
                .len()
                .cmp(&iter_b.len())
                .reverse()
                .then_with(|| iter_a.cmp(iter_b)),
        )
    }
}

impl<'br, BL, BR> PartialOrd<SubtreePathBuilder<'br, BR>> for SubtreePath<'_, BL>
where
    BL: AsRef<[u8]>,
    BR: AsRef<[u8]>,
{
    fn partial_cmp(&self, other: &SubtreePathBuilder<'br, BR>) -> Option<cmp::Ordering> {
        self.partial_cmp(&SubtreePath::from(other))
    }
}

impl<BL> Ord for SubtreePath<'_, BL>
where
    BL: AsRef<[u8]>,
{
    fn cmp(&self, other: &Self) -> cmp::Ordering {
        self.partial_cmp(other).expect("order is totally defined")
    }
}

impl<BL> Ord for SubtreePathBuilder<'_, BL>
where
    BL: AsRef<[u8]>,
{
    fn cmp(&self, other: &Self) -> cmp::Ordering {
        self.partial_cmp(other).expect("order is totally defined")
    }
}

impl<B: AsRef<[u8]>> Eq for SubtreePath<'_, B> {}

impl<'b, B> From<SubtreePathInner<'b, B>> for SubtreePath<'b, B> {
    fn from(ref_variant: SubtreePathInner<'b, B>) -> Self {
        Self { ref_variant }
    }
}

impl<'b, B> From<&'b [B]> for SubtreePath<'b, B> {
    fn from(value: &'b [B]) -> Self {
        SubtreePathInner::Slice(value).into()
    }
}

impl<'b, B, const N: usize> From<&'b [B; N]> for SubtreePath<'b, B> {
    fn from(value: &'b [B; N]) -> Self {
        SubtreePathInner::Slice(value).into()
    }
}

/// Create a link to existing [SubtreePath] that cannot outlive it, because it
/// possibly owns some of the path segments.
impl<'s, 'b, B> From<&'s SubtreePathBuilder<'b, B>> for SubtreePath<'s, B> {
    fn from(value: &'s SubtreePathBuilder<'b, B>) -> Self {
        SubtreePathInner::SubtreePath(value).into()
    }
}

/// Hash order is the same as iteration order: from most deep path segment up to
/// root.
impl<B: AsRef<[u8]>> Hash for SubtreePath<'_, B> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        match &self.ref_variant {
            SubtreePathInner::Slice(slice) => slice
                .iter()
                .map(AsRef::as_ref)
                .rev()
                .for_each(|s| s.hash(state)),
            SubtreePathInner::SubtreePath(path) => path.hash(state),
            SubtreePathInner::SubtreePathIter(path_iter) => {
                path_iter.clone().for_each(|s| s.hash(state))
            }
        }
    }
}

/// For the same reason as for `Hash` implementation, derived impl requires
/// generics to carry trait bounds that actually don't needed.
impl<B> Clone for SubtreePath<'_, B> {
    fn clone(&self) -> Self {
        match &self.ref_variant {
            SubtreePathInner::Slice(x) => SubtreePathInner::Slice(x),
            SubtreePathInner::SubtreePath(x) => SubtreePathInner::SubtreePath(x),
            SubtreePathInner::SubtreePathIter(x) => SubtreePathInner::SubtreePathIter(x.clone()),
        }
        .into()
    }
}

impl SubtreePath<'static, [u8; 0]> {
    /// Get empty subtree path (meaning it'll point to the root tree).
    pub const fn empty() -> Self {
        SubtreePath {
            ref_variant: SubtreePathInner::Slice(&[]),
        }
    }
}

impl<B> SubtreePath<'_, B> {
    /// Returns the length of the subtree path.
    pub fn len(&self) -> usize {
        match &self.ref_variant {
            SubtreePathInner::Slice(s) => s.len(),
            SubtreePathInner::SubtreePath(path) => path.len(),
            SubtreePathInner::SubtreePathIter(path_iter) => path_iter.len(),
        }
    }

    /// Returns whether the path is empty (the root tree).
    pub fn is_empty(&self) -> bool {
        match &self.ref_variant {
            SubtreePathInner::Slice(s) => s.is_empty(),
            SubtreePathInner::SubtreePath(path) => path.is_empty(),
            SubtreePathInner::SubtreePathIter(path_iter) => path_iter.is_empty(),
        }
    }
}

impl<'b, B: AsRef<[u8]>> SubtreePath<'b, B> {
    /// Get a derived path that will reuse this [Self] as it's base path and
    /// capable of owning data.
    pub fn derive_owned(&self) -> SubtreePathBuilder<'b, B> {
        self.into()
    }

    /// Get a derived path with a child path segment added.
    pub fn derive_owned_with_child<'s, S>(&self, segment: S) -> SubtreePathBuilder<'b, B>
    where
        S: Into<CowLike<'s>>,
        's: 'b,
    {
        SubtreePathBuilder {
            base: self.clone(),
            relative: SubtreePathRelative::Single(segment.into()),
        }
    }

    /// Get a derived subtree path for a parent with care for base path slice
    /// case. The main difference from [SubtreePath::derive_parent] is that
    /// lifetime of returned [Self] if not limited to the scope where this
    /// function was called so it's possible to follow to ancestor paths
    /// without keeping previous result as it still will link to `'b`
    /// (latest [SubtreePath] or initial slice of data).
    pub fn derive_parent(&self) -> Option<(SubtreePath<'b, B>, &'b [u8])> {
        match &self.ref_variant {
            SubtreePathInner::Slice(path) => path
                .split_last()
                .map(|(tail, rest)| (SubtreePathInner::Slice(rest).into(), tail.as_ref())),
            SubtreePathInner::SubtreePath(path) => path.derive_parent(),
            SubtreePathInner::SubtreePathIter(iter) => {
                let mut derived_iter = iter.clone();
                derived_iter.next().map(|segment| {
                    (
                        SubtreePathInner::SubtreePathIter(derived_iter).into(),
                        segment,
                    )
                })
            }
        }
    }

    /// Get a reverse path segments iterator.
    pub fn into_reverse_iter(self) -> SubtreePathIter<'b, B> {
        match self.ref_variant {
            SubtreePathInner::Slice(slice) => SubtreePathIter::new(slice.iter()),
            SubtreePathInner::SubtreePath(path) => path.reverse_iter(),
            SubtreePathInner::SubtreePathIter(iter) => iter,
        }
    }

    /// Retuns `true` if the subtree path is empty, so it points to the root
    /// tree.
    pub fn is_root(&self) -> bool {
        match &self.ref_variant {
            SubtreePathInner::Slice(s) => s.is_empty(),
            SubtreePathInner::SubtreePath(path) => path.is_root(),
            SubtreePathInner::SubtreePathIter(iter) => iter.is_empty(),
        }
    }

    /// Collect path as a vector of vectors, but this actually negates all the
    /// benefits of this library.
    pub fn to_vec(&self) -> Vec<Vec<u8>> {
        match &self.ref_variant {
            SubtreePathInner::Slice(slice) => slice.iter().map(|x| x.as_ref().to_vec()).collect(),
            SubtreePathInner::SubtreePath(path) => path.to_vec(),
            SubtreePathInner::SubtreePathIter(iter) => {
                let mut path = iter
                    .clone()
                    .map(|x| x.as_ref().to_vec())
                    .collect::<Vec<Vec<u8>>>();
                path.reverse();
                path
            }
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
        builder.push_segment(b"eight");
        let parent = builder.derive_parent().unwrap().0;

        let as_vec = parent.to_vec();
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
        assert_eq!(parent.len(), reference_vec.len());
    }

    #[test]
    fn ordering() {
        let path_a: SubtreePath<_> = (&[b"one" as &[u8], b"two", b"three"]).into();
        let path_b = path_a.derive_owned_with_child(b"four");
        let path_c = path_a.derive_owned_with_child(b"notfour");
        let (path_d_parent, _) = path_a.derive_parent().unwrap();
        let path_d = path_d_parent.derive_owned_with_child(b"three");

        // Same lengths for different paths don't make them equal:
        assert!(!matches!(
            SubtreePath::from(&path_b).cmp(&SubtreePath::from(&path_c)),
            cmp::Ordering::Equal
        ));

        // Equal paths made the same way are equal:
        assert!(matches!(
            path_a.cmp(&SubtreePath::from(&path_d)),
            cmp::Ordering::Equal
        ));

        // Longer paths come first
        assert!(path_a > path_b);
        assert!(path_a > path_c);
    }
}
