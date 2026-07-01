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

//! Reverse iterator for a subtree path definition and implementation.

use std::slice;

use crate::{subtree_path::SubtreePath, util::CompactBytesIter};

/// (Reverse) iterator for a subtree path.
/// Because of implementation details (one way link between derivations) it
/// cannot effectively iterate from the most shallow path segment to the
/// deepest, so it have to go in reverse direction.
#[derive(Debug)]
pub struct SubtreePathIter<'b, B> {
    current_iter: CurrentSubtreePathIter<'b, B>,
    next_subtree_path: Option<&'b SubtreePath<'b, B>>,
}

impl<B> Clone for SubtreePathIter<'_, B> {
    fn clone(&self) -> Self {
        SubtreePathIter {
            current_iter: self.current_iter.clone(),
            next_subtree_path: self.next_subtree_path,
        }
    }
}

impl<'b, B> SubtreePathIter<'b, B> {
    pub(crate) fn len(&self) -> usize {
        self.current_iter.len() + self.next_subtree_path.map(|p| p.len()).unwrap_or_default()
    }

    pub(crate) fn new<I>(iter: I) -> Self
    where
        I: Into<CurrentSubtreePathIter<'b, B>>,
    {
        SubtreePathIter {
            current_iter: iter.into(),
            next_subtree_path: None,
        }
    }

    pub(crate) fn new_with_next<I>(iter: I, next: &'b SubtreePath<'b, B>) -> Self
    where
        I: Into<CurrentSubtreePathIter<'b, B>>,
    {
        SubtreePathIter {
            current_iter: iter.into(),
            next_subtree_path: Some(next),
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.next_subtree_path.is_none()
            && match &self.current_iter {
                CurrentSubtreePathIter::Single(_) => false,
                CurrentSubtreePathIter::Slice(slice) => slice.len() == 0,
                CurrentSubtreePathIter::OwnedBytes(bytes_iter) => bytes_iter.len() == 0,
            }
    }
}

impl<'b, B: AsRef<[u8]>> Iterator for SubtreePathIter<'b, B> {
    type Item = &'b [u8];

    fn next(&mut self) -> Option<Self::Item> {
        match &mut self.current_iter {
            CurrentSubtreePathIter::Single(item) => {
                let path_segment = *item;
                if let Some(next_path) = self.next_subtree_path {
                    *self = next_path.clone().into_reverse_iter();
                }
                Some(path_segment)
            }
            CurrentSubtreePathIter::Slice(slice_iter) => {
                if let Some(item) = slice_iter.next_back() {
                    Some(item.as_ref())
                } else if let Some(next_path) = self.next_subtree_path {
                    *self = next_path.clone().into_reverse_iter();
                    self.next()
                } else {
                    None
                }
            }
            CurrentSubtreePathIter::OwnedBytes(bytes_iter) => {
                if let Some(item) = bytes_iter.next() {
                    Some(item)
                } else if let Some(next_path) = self.next_subtree_path {
                    *self = next_path.clone().into_reverse_iter();
                    self.next()
                } else {
                    None
                }
            }
        }
    }
}

/// An iterator variant depending on how the current subtree path's derivation
/// point looks like.
#[derive(Debug)]
pub(crate) enum CurrentSubtreePathIter<'b, B> {
    /// Current derivation point is a [SubtreePathBuilder] with one child
    /// segment
    Single(&'b [u8]),
    /// Current (and last) part of the subtree path is a base slice of data,
    /// will just reuse slice's iterator there
    Slice(slice::Iter<'b, B>),
    /// Current derivation point is a [SubtreePathBuilder] with multiple path
    /// segments, will reuse it's own iterator type to keep track
    OwnedBytes(CompactBytesIter<'b>),
}

impl<B> CurrentSubtreePathIter<'_, B> {
    pub fn len(&self) -> usize {
        match self {
            CurrentSubtreePathIter::Single(_) => 1,
            CurrentSubtreePathIter::Slice(s) => s.len(),
            CurrentSubtreePathIter::OwnedBytes(cb) => cb.len(),
        }
    }
}

impl<B> Clone for CurrentSubtreePathIter<'_, B> {
    fn clone(&self) -> Self {
        match self {
            CurrentSubtreePathIter::Single(x) => CurrentSubtreePathIter::Single(x),
            CurrentSubtreePathIter::Slice(x) => CurrentSubtreePathIter::Slice(x.clone()),
            CurrentSubtreePathIter::OwnedBytes(x) => CurrentSubtreePathIter::OwnedBytes(*x),
        }
    }
}

impl<'b, B> From<CompactBytesIter<'b>> for CurrentSubtreePathIter<'b, B> {
    fn from(value: CompactBytesIter<'b>) -> Self {
        CurrentSubtreePathIter::<B>::OwnedBytes(value)
    }
}

impl<'b, B> From<slice::Iter<'b, B>> for CurrentSubtreePathIter<'b, B> {
    fn from(value: slice::Iter<'b, B>) -> Self {
        CurrentSubtreePathIter::Slice(value)
    }
}

impl<'b, B> From<&'b [u8]> for CurrentSubtreePathIter<'b, B> {
    fn from(value: &'b [u8]) -> Self {
        CurrentSubtreePathIter::Single(value)
    }
}
