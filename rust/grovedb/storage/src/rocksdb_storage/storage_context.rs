// MIT LICENSE
//
// Copyright (c) 2021 Dash Core Group
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

//! Implementation of prefixed storage context.

mod batch;
pub mod context_immediate;
mod context_tx;
mod raw_iterator;

pub use batch::PrefixedRocksDbBatch;
pub use context_immediate::PrefixedRocksDbImmediateStorageContext;
pub use context_tx::PrefixedRocksDbTransactionContext;
pub use raw_iterator::PrefixedRocksDbRawIterator;

use super::storage::SubtreePrefix;

/// Make prefixed key
pub fn make_prefixed_key<K: AsRef<[u8]>>(prefix: &SubtreePrefix, key: K) -> Vec<u8> {
    let mut prefix_vec = prefix.to_vec();
    prefix_vec.extend_from_slice(key.as_ref());
    prefix_vec
}
