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

use std::{
    cmp::Ordering,
    ops::{Add, AddAssign},
};

use integer_encoding::VarInt;

use crate::{
    storage_cost::{removal::StorageRemovedBytes::NoStorageRemoval, StorageCost},
    BasicStorageRemoval, StorageRemovedBytes,
};

/// Storage only operation costs separated by key and value
#[derive(PartialEq, Clone, Eq, Default)]
pub struct KeyValueStorageCost {
    /// Key storage_cost costs
    pub key_storage_cost: StorageCost,
    /// Value storage_cost costs
    pub value_storage_cost: StorageCost,
    /// Is this a new node
    pub new_node: bool,
    /// Should we verify this at storage time
    pub needs_value_verification: bool,
}

impl KeyValueStorageCost {
    /// Convenience method for getting the cost of updating the key of the root
    /// of each merk
    pub fn for_updated_root_cost(old_tree_key_len: Option<u32>, tree_key_len: u32) -> Self {
        if let Some(old_tree_key_len) = old_tree_key_len {
            let key_storage_cost = StorageCost {
                added_bytes: 0,
                replaced_bytes: 34, // prefix + 1 for 'r' + 1 required space
                removed_bytes: NoStorageRemoval,
            };
            let new_bytes = tree_key_len + tree_key_len.required_space() as u32;
            let value_storage_cost = match tree_key_len.cmp(&old_tree_key_len) {
                Ordering::Less => {
                    // we removed bytes
                    let old_bytes = old_tree_key_len + old_tree_key_len.required_space() as u32;
                    StorageCost {
                        added_bytes: 0,
                        replaced_bytes: new_bytes,
                        removed_bytes: BasicStorageRemoval(old_bytes - new_bytes),
                    }
                }
                Ordering::Equal => StorageCost {
                    added_bytes: 0,
                    replaced_bytes: new_bytes,
                    removed_bytes: NoStorageRemoval,
                },
                Ordering::Greater => {
                    let old_bytes = old_tree_key_len + old_tree_key_len.required_space() as u32;
                    StorageCost {
                        added_bytes: new_bytes - old_bytes,
                        replaced_bytes: old_bytes,
                        removed_bytes: NoStorageRemoval,
                    }
                }
            };
            KeyValueStorageCost {
                key_storage_cost,
                value_storage_cost,
                new_node: false,
                needs_value_verification: false,
            }
        } else {
            KeyValueStorageCost {
                key_storage_cost: StorageCost {
                    added_bytes: 34, // prefix + 1 for 'r' + 1 required space
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                value_storage_cost: StorageCost {
                    added_bytes: tree_key_len + tree_key_len.required_space() as u32,
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                new_node: true,
                needs_value_verification: false,
            }
        }
    }

    /// Returns the total removed bytes between the key removed bytes and the
    /// value removed bytes
    pub fn combined_removed_bytes(self) -> StorageRemovedBytes {
        self.key_storage_cost.removed_bytes + self.value_storage_cost.removed_bytes
    }
}

impl Add for KeyValueStorageCost {
    type Output = Self;

    fn add(self, rhs: Self) -> Self::Output {
        Self {
            key_storage_cost: self.key_storage_cost + rhs.key_storage_cost,
            value_storage_cost: self.value_storage_cost + rhs.value_storage_cost,
            new_node: self.new_node & rhs.new_node,
            needs_value_verification: self.needs_value_verification & rhs.needs_value_verification,
        }
    }
}

impl AddAssign for KeyValueStorageCost {
    fn add_assign(&mut self, rhs: Self) {
        self.key_storage_cost += rhs.key_storage_cost;
        self.value_storage_cost += rhs.value_storage_cost;
        self.new_node &= rhs.new_node;
        self.needs_value_verification &= rhs.needs_value_verification;
    }
}
