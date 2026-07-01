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

use std::ops::{Add, AddAssign};

use crate::{
    error::Error,
    storage_cost::removal::{StorageRemovedBytes, StorageRemovedBytes::NoStorageRemoval},
};

/// Key Value Storage Costs
pub mod key_value_cost;
/// Storage Removal
pub mod removal;
/// Costs to Transitions
pub mod transition;

/// Storage only operation costs
#[derive(Debug, PartialEq, Clone, Eq)]
pub struct StorageCost {
    /// How many bytes are said to be added on hard drive.
    pub added_bytes: u32,
    /// How many bytes are said to be replaced on hard drive.
    pub replaced_bytes: u32,
    /// How many bytes are said to be removed on hard drive.
    pub removed_bytes: StorageRemovedBytes,
}

impl Add for StorageCost {
    type Output = Self;

    fn add(self, rhs: Self) -> Self::Output {
        Self {
            added_bytes: self.added_bytes + rhs.added_bytes,
            replaced_bytes: self.replaced_bytes + rhs.replaced_bytes,
            removed_bytes: self.removed_bytes + rhs.removed_bytes,
        }
    }
}

impl AddAssign for StorageCost {
    fn add_assign(&mut self, rhs: Self) {
        self.added_bytes += rhs.added_bytes;
        self.replaced_bytes += rhs.replaced_bytes;
        self.removed_bytes += rhs.removed_bytes;
    }
}

impl StorageCost {
    /// Verify that the len of the item matches the given storage_cost cost
    pub fn verify(&self, len: u32) -> Result<(), Error> {
        if self.added_bytes + self.replaced_bytes == len {
            Ok(())
        } else {
            Err(Error::StorageCostMismatch {
                expected: self.clone(),
                actual_total_bytes: len,
            })
        }
    }

    /// Verifies the len of a key item only if the node is new
    /// doesn't need to verify for the update case since the key never changes
    pub fn verify_key_storage_cost(&self, len: u32, new_node: bool) -> Result<(), Error> {
        if new_node {
            self.verify(len)
        } else {
            Ok(())
        }
    }

    /// worse_or_eq_than means worse for things that would cost resources
    /// storage_freed_bytes is worse when it is lower instead
    pub fn worse_or_eq_than(&self, other: &Self) -> bool {
        self.replaced_bytes >= other.replaced_bytes
            && self.added_bytes >= other.added_bytes
            && self.removed_bytes <= other.removed_bytes
    }

    /// are the replaced bytes or removed bytes different than 0?
    pub fn has_storage_change(&self) -> bool {
        self.added_bytes != 0 || self.removed_bytes.total_removed_bytes() != 0
    }
}

impl Default for StorageCost {
    fn default() -> Self {
        Self {
            added_bytes: 0,
            replaced_bytes: 0,
            removed_bytes: NoStorageRemoval,
        }
    }
}
