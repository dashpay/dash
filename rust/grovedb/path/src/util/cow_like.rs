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

//! Module for [CowLike]: simple abstraction over owned and borrowed bytes.

use std::{
    hash::{Hash, Hasher},
    ops::Deref,
};

/// A smart pointer that follows the semantics of [Cow](std::borrow::Cow) except
/// provides no means for mutability and thus doesn't require [Clone].
#[derive(Debug, Clone)]
pub enum CowLike<'b> {
    Owned(Vec<u8>),
    Borrowed(&'b [u8]),
}

impl Deref for CowLike<'_> {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        match self {
            Self::Owned(v) => v.as_slice(),
            Self::Borrowed(s) => s,
        }
    }
}

impl AsRef<[u8]> for CowLike<'_> {
    fn as_ref(&self) -> &[u8] {
        self
    }
}

impl Hash for CowLike<'_> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.deref().hash(state);
    }
}

impl From<Vec<u8>> for CowLike<'static> {
    fn from(value: Vec<u8>) -> Self {
        Self::Owned(value)
    }
}

impl<'b> From<&'b [u8]> for CowLike<'b> {
    fn from(value: &'b [u8]) -> Self {
        Self::Borrowed(value)
    }
}

impl<'b, const N: usize> From<&'b [u8; N]> for CowLike<'b> {
    fn from(value: &'b [u8; N]) -> Self {
        Self::Borrowed(value.as_ref())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::calculate_hash;

    #[test]
    fn test_cowlike_hashes() {
        let owned = CowLike::Owned(vec![1u8, 3, 3, 7]);
        let borrowed = CowLike::Borrowed(&[1u8, 3, 3, 7]);

        assert_eq!(calculate_hash(&owned), calculate_hash(&borrowed));
    }
}
