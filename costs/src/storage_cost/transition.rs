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

use crate::storage_cost::{
    transition::OperationStorageTransitionType::{
        OperationDelete, OperationInsertNew, OperationNone, OperationReplace,
        OperationUpdateBiggerSize, OperationUpdateSameSize, OperationUpdateSmallerSize,
    },
    StorageCost,
};

/// Based off of storage_cost changes what type of transition has occurred?
pub enum OperationStorageTransitionType {
    /// An element that didn't exist before was inserted
    OperationInsertNew,
    /// An element that existed was updated and was made bigger
    OperationUpdateBiggerSize,
    /// An element that existed was updated and was made smaller
    OperationUpdateSmallerSize,
    /// An element that existed was updated, but stayed the same size
    OperationUpdateSameSize,
    /// An element was replaced, this can happen if an insertion operation was
    /// marked as a replacement An example would be if User A added
    /// something, an User B replaced it. User A should get their value in
    /// credits back, User B should pay as if an insert
    OperationReplace,
    /// An element was deleted
    OperationDelete,
    /// Nothing happened
    OperationNone,
}

impl StorageCost {
    /// the type of transition that the costs represent
    pub fn transition_type(&self) -> OperationStorageTransitionType {
        if self.added_bytes > 0 {
            if self.removed_bytes.has_removal() {
                OperationReplace
            } else if self.replaced_bytes > 0 {
                OperationUpdateBiggerSize
            } else {
                OperationInsertNew
            }
        } else if self.removed_bytes.has_removal() {
            if self.replaced_bytes > 0 {
                OperationUpdateSmallerSize
            } else {
                OperationDelete
            }
        } else if self.replaced_bytes > 0 {
            OperationUpdateSameSize
        } else {
            OperationNone
        }
    }
}
