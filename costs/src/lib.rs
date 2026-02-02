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

#![deny(missing_docs)]

//! Interface crate to unify how operations' costs are passed and retrieved.

/// Cost Contexts
pub mod context;
/// Cost Errors
pub mod error;
/// Storage Costs
pub mod storage_cost;

use std::ops::{Add, AddAssign};

pub use context::{CostContext, CostResult, CostsExt};
use integer_encoding::VarInt;

use crate::{
    error::Error,
    storage_cost::{
        key_value_cost::KeyValueStorageCost, removal::StorageRemovedBytes, StorageCost,
    },
    StorageRemovedBytes::BasicStorageRemoval,
};

/// Child key length
pub type ChildKeyLength = u32;

/// Feature sum length
pub type FeatureSumLength = u32;

/// Child sum length
pub type ChildSumLength = u32;

/// Children sizes
pub type ChildrenSizes = Option<(
    Option<(ChildKeyLength, ChildSumLength)>,
    Option<(ChildKeyLength, ChildSumLength)>,
)>;

/// Children sizes starting with a value
pub type ChildrenSizesWithValue = Option<(
    Vec<u8>,
    Option<(ChildKeyLength, ChildSumLength)>,
    Option<(ChildKeyLength, ChildSumLength)>,
)>;

/// The tree cost type
pub enum TreeCostType {
    /// This is for sum trees and count trees
    TreeFeatureUsesVarIntCostAs8Bytes,
    /// This is for count sum trees
    TreeFeatureUsesTwoVarIntsCostAs16Bytes,
    /// This is for big sum trees
    TreeFeatureUses16Bytes,
}

impl TreeCostType {
    fn cost_size(&self) -> u32 {
        match self {
            TreeCostType::TreeFeatureUsesVarIntCostAs8Bytes => 8,
            TreeCostType::TreeFeatureUsesTwoVarIntsCostAs16Bytes => 16,
            TreeCostType::TreeFeatureUses16Bytes => 16,
        }
    }
}

/// Children sizes starting with if we are in a sum tree
pub type ChildrenSizesWithIsSumTree = Option<(
    Option<(TreeCostType, FeatureSumLength)>,
    Option<(ChildKeyLength, ChildSumLength)>,
    Option<(ChildKeyLength, ChildSumLength)>,
)>;

/// Piece of data representing affected computer resources (approximately).
#[derive(Debug, Default, Eq, PartialEq, Clone)]
pub struct OperationCost {
    /// How many storage_cost seeks were done.
    pub seek_count: u32,
    /// Storage cost of the operation.
    pub storage_cost: StorageCost,
    /// How many bytes were loaded from hard drive.
    pub storage_loaded_bytes: u64,
    /// How many times node hashing was done (for merkelized tree).
    pub hash_node_calls: u32,
}

impl OperationCost {
    /// Is Nothing
    pub fn is_nothing(&self) -> bool {
        self == &Self::default()
    }

    /// Helper function to build default `OperationCost` with different
    /// `seek_count`.
    pub fn with_seek_count(seek_count: u32) -> Self {
        OperationCost {
            seek_count,
            ..Default::default()
        }
    }

    /// Helper function to build default `OperationCost` with different
    /// `storage_written_bytes`.
    pub fn with_storage_written_bytes(storage_written_bytes: u32) -> Self {
        OperationCost {
            storage_cost: StorageCost {
                added_bytes: storage_written_bytes,
                ..Default::default()
            },
            ..Default::default()
        }
    }

    /// Helper function to build default `OperationCost` with different
    /// `storage_loaded_bytes`.
    pub fn with_storage_loaded_bytes(storage_loaded_bytes: u64) -> Self {
        OperationCost {
            storage_loaded_bytes,
            ..Default::default()
        }
    }

    /// Helper function to build default `OperationCost` with different
    /// `storage_freed_bytes`.
    pub fn with_storage_freed_bytes(storage_freed_bytes: u32) -> Self {
        OperationCost {
            storage_cost: StorageCost {
                removed_bytes: BasicStorageRemoval(storage_freed_bytes),
                ..Default::default()
            },
            ..Default::default()
        }
    }

    /// Helper function to build default `OperationCost` with different
    /// `hash_node_calls`.
    pub fn with_hash_node_calls(hash_node_calls: u32) -> Self {
        OperationCost {
            hash_node_calls,
            ..Default::default()
        }
    }

    /// worse_or_eq_than means worse for things that would cost resources
    /// storage_freed_bytes is worse when it is lower instead
    pub fn worse_or_eq_than(&self, other: &Self) -> bool {
        self.seek_count >= other.seek_count
            && self.storage_cost.worse_or_eq_than(&other.storage_cost)
            && self.storage_loaded_bytes >= other.storage_loaded_bytes
            && self.hash_node_calls >= other.hash_node_calls
    }

    /// add storage_cost costs for key and value storages
    pub fn add_key_value_storage_costs(
        &mut self,
        key_len: u32,
        value_len: u32,
        children_sizes: ChildrenSizesWithIsSumTree,
        storage_cost_info: Option<KeyValueStorageCost>,
    ) -> Result<(), Error> {
        let paid_key_len = key_len + key_len.required_space() as u32;

        let doesnt_need_verification = storage_cost_info
            .as_ref()
            .map(|key_value_storage_cost| {
                if !key_value_storage_cost.needs_value_verification {
                    Some(
                        key_value_storage_cost.value_storage_cost.added_bytes
                            + key_value_storage_cost.value_storage_cost.replaced_bytes,
                    )
                } else {
                    None
                }
            })
            .unwrap_or(None);
        let final_paid_value_len = if let Some(value_cost_len) = doesnt_need_verification {
            value_cost_len
        } else {
            let mut paid_value_len = value_len;
            // We need to remove the child sizes if they exist
            if let Some((in_sum_tree, left_child, right_child)) = children_sizes {
                paid_value_len -= 2; // for the child options

                // We need to remove the costs of the children
                if let Some((left_child_len, left_child_sum_len)) = left_child {
                    paid_value_len -= left_child_len;
                    paid_value_len -= left_child_sum_len;
                }
                if let Some((right_child_len, right_child_sum_len)) = right_child {
                    paid_value_len -= right_child_len;
                    paid_value_len -= right_child_sum_len;
                }

                let sum_tree_node_size = if let Some((tree_cost_type, sum_tree_len)) = in_sum_tree {
                    let cost_size = tree_cost_type.cost_size();
                    paid_value_len -= sum_tree_len;
                    paid_value_len += cost_size;
                    cost_size
                } else {
                    0
                };

                // This is the moment we need to add the required space (after removing
                // children) but before adding the parent to child hook
                paid_value_len += paid_value_len.required_space() as u32;

                // Now we are the parent to child hook

                // We need to add the cost of a parent
                // key_len has a hash length already in it from the key prefix
                // So we need to remove it and then add a hash length
                // For the parent ref + 4 (2 for child sizes, 1 for key_len, 1 for sum option)

                paid_value_len += key_len + 4 + sum_tree_node_size;
            } else {
                paid_value_len += paid_value_len.required_space() as u32;
            }
            paid_value_len
        };

        let (key_storage_cost, value_storage_costs) = match storage_cost_info {
            None => (None, None),
            Some(s) => {
                s.key_storage_cost
                    .verify_key_storage_cost(paid_key_len, s.new_node)?;
                s.value_storage_cost.verify(final_paid_value_len)?;
                (Some(s.key_storage_cost), Some(s.value_storage_cost))
            }
        };

        self.add_storage_costs(paid_key_len, key_storage_cost);
        self.add_storage_costs(final_paid_value_len, value_storage_costs);
        Ok(())
    }

    /// add_storage_costs adds storage_cost costs for a key or a value
    fn add_storage_costs(
        &mut self,
        len_with_required_space: u32,
        storage_cost_info: Option<StorageCost>,
    ) {
        match storage_cost_info {
            // There is no storage_cost cost info, just use value len
            None => {
                self.storage_cost += StorageCost {
                    added_bytes: len_with_required_space,
                    ..Default::default()
                }
            }
            Some(storage_cost) => {
                self.storage_cost += storage_cost;
            }
        }
    }
}

impl Add for OperationCost {
    type Output = Self;

    fn add(self, rhs: Self) -> Self::Output {
        OperationCost {
            seek_count: self.seek_count + rhs.seek_count,
            storage_cost: self.storage_cost + rhs.storage_cost,
            storage_loaded_bytes: self.storage_loaded_bytes + rhs.storage_loaded_bytes,
            hash_node_calls: self.hash_node_calls + rhs.hash_node_calls,
        }
    }
}

impl AddAssign for OperationCost {
    fn add_assign(&mut self, rhs: Self) {
        self.seek_count += rhs.seek_count;
        self.storage_cost += rhs.storage_cost;
        self.storage_loaded_bytes += rhs.storage_loaded_bytes;
        self.hash_node_calls += rhs.hash_node_calls;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::context::{CostContext, CostResult, CostsExt};

    #[test]
    fn test_map() {
        let initial = CostContext {
            value: 75,
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };

        let mapped = initial.map(|x| x + 25);
        assert_eq!(
            mapped,
            CostContext {
                value: 100,
                cost: OperationCost {
                    storage_loaded_bytes: 3,
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn test_flat_map() {
        let initial = CostContext {
            value: 75,
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };

        let mapped = initial.flat_map(|x| CostContext {
            value: x + 25,
            cost: OperationCost {
                storage_loaded_bytes: 7,
                ..Default::default()
            },
        });
        assert_eq!(
            mapped,
            CostContext {
                value: 100,
                cost: OperationCost {
                    storage_loaded_bytes: 10,
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn test_map_ok() {
        let initial: CostResult<usize, ()> = CostContext {
            value: Ok(75),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };

        let mapped = initial.map_ok(|x| x + 25);
        assert_eq!(
            mapped,
            CostContext {
                value: Ok(100),
                cost: OperationCost {
                    storage_loaded_bytes: 3,
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn test_map_ok_err() {
        let initial: CostResult<usize, ()> = CostContext {
            value: Err(()),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };

        let mapped = initial.map_ok(|x| x + 25);
        assert_eq!(
            mapped,
            CostContext {
                value: Err(()),
                cost: OperationCost {
                    storage_loaded_bytes: 3,
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn test_flat_map_ok() {
        let initial: CostResult<usize, ()> = CostContext {
            value: Ok(75),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };

        let mapped = initial.flat_map_ok(|x| CostContext {
            value: Ok(x + 25),
            cost: OperationCost {
                storage_loaded_bytes: 7,
                ..Default::default()
            },
        });
        assert_eq!(
            mapped,
            CostContext {
                value: Ok(100),
                cost: OperationCost {
                    storage_loaded_bytes: 10,
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn test_flat_map_err_first() {
        let initial: CostResult<usize, ()> = CostContext {
            value: Err(()),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };
        let mut executed = false;
        let mapped = initial.flat_map_ok(|x| {
            executed = true;
            CostContext {
                value: Ok(x + 25),
                cost: OperationCost {
                    storage_loaded_bytes: 7,
                    ..Default::default()
                },
            }
        });

        // Second function won't be executed and thus no costs added.
        assert!(!executed);
        assert_eq!(
            mapped,
            CostContext {
                value: Err(()),
                cost: OperationCost {
                    storage_loaded_bytes: 3,
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn test_flat_map_err_second() {
        let initial: CostResult<usize, ()> = CostContext {
            value: Ok(75),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };
        let mut executed = false;
        let mapped: CostResult<usize, ()> = initial.flat_map_ok(|_| {
            executed = true;
            CostContext {
                value: Err(()),
                cost: OperationCost {
                    storage_loaded_bytes: 7,
                    ..Default::default()
                },
            }
        });

        // Second function should be executed and costs should increase. Result is error
        // though.
        assert!(executed);
        assert_eq!(
            mapped,
            CostContext {
                value: Err(()),
                cost: OperationCost {
                    storage_loaded_bytes: 10,
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn test_flatten_nested_errors() {
        let initial: CostResult<usize, &str> = CostContext {
            value: Ok(75),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };
        // We use function that has nothing to do with costs but returns a result, we're
        // trying to flatten nested errors inside CostContext.
        let ok = initial.map_ok(|x| Ok(x + 25));
        assert_eq!(ok.flatten().unwrap(), Ok(100));

        let initial: CostResult<usize, &str> = CostContext {
            value: Ok(75),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };
        let error_inner: CostResult<Result<usize, &str>, &str> = initial.map_ok(|_| Err("latter"));
        assert_eq!(error_inner.flatten().unwrap(), Err("latter"));

        let initial: CostResult<usize, &str> = CostContext {
            value: Err("inner"),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };
        let error_inner: CostResult<Result<usize, &str>, &str> = initial.map_ok(|x| Ok(x + 25));
        assert_eq!(error_inner.flatten().unwrap(), Err("inner"));
    }

    #[test]
    fn test_wrap_fn_cost() {
        // Imagine this one is loaded from storage.
        let loaded_value = b"ayylmao";
        let costs_ctx = loaded_value.wrap_fn_cost(|x| OperationCost {
            seek_count: 1,
            storage_loaded_bytes: x.len() as u64,
            ..Default::default()
        });
        assert_eq!(
            costs_ctx,
            CostContext {
                value: loaded_value,
                cost: OperationCost {
                    seek_count: 1,
                    storage_loaded_bytes: 7,
                    ..Default::default()
                }
            }
        )
    }

    #[test]
    fn test_map_err() {
        let initial: CostResult<usize, ()> = CostContext {
            value: Err(()),
            cost: OperationCost {
                storage_loaded_bytes: 3,
                ..Default::default()
            },
        };

        let mapped = initial.map_err(|_| "ayyerror");
        assert_eq!(
            mapped,
            CostContext {
                value: Err("ayyerror"),
                cost: OperationCost {
                    storage_loaded_bytes: 3,
                    ..Default::default()
                },
            }
        );
    }
}
