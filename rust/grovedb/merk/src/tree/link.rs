//! Merk tree link

#[cfg(feature = "minimal")]
use std::io::{Read, Write};

#[cfg(feature = "minimal")]
use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
#[cfg(feature = "minimal")]
use ed::{Decode, Encode, Result, Terminated};
#[cfg(feature = "minimal")]
use integer_encoding::{VarInt, VarIntReader, VarIntWriter};

#[cfg(feature = "minimal")]
use super::{hash::CryptoHash, TreeNode};
#[cfg(feature = "minimal")]
use crate::merk::NodeType;
#[cfg(feature = "minimal")]
use crate::tree::tree_feature_type::AggregateData;
#[cfg(feature = "minimal")]
use crate::HASH_LENGTH_U32;
// TODO: optimize memory footprint

#[cfg(feature = "minimal")]
/// Represents a reference to a child tree node. Links may or may not contain
/// the child's `Tree` instance (storing its key if not).
#[derive(Clone, Debug, PartialEq)]
pub enum Link {
    /// Represents a child tree node which has been pruned from memory, only
    /// retaining a reference to it (its key). The child node can always be
    /// fetched from the backing store by this key when necessary.
    Reference {
        /// Hash
        hash: CryptoHash,
        /// Child heights
        child_heights: (u8, u8),
        /// Key
        key: Vec<u8>,
        /// Aggregate data like Sum
        aggregate_data: AggregateData,
    },

    /// Represents a tree node which has been modified since the `Tree`'s last
    /// hash computation. The child's hash is not stored since it has not yet
    /// been recomputed. The child's `Tree` instance is stored in the link.
    #[rustfmt::skip]
    Modified {
        /// Pending writes
        pending_writes: usize, // TODO: rename to `pending_hashes`
        /// Child heights
        child_heights: (u8, u8),
        /// Tree
        tree: TreeNode
    },

    /// Represents a tree node which has been modified since the `Tree`'s last
    /// commit, but which has an up-to-date hash. The child's `Tree` instance is
    /// stored in the link.
    Uncommitted {
        /// Hash
        hash: CryptoHash,
        /// Child heights
        child_heights: (u8, u8),
        /// Tree
        tree: TreeNode,
        /// Aggregate data like Sum
        aggregate_data: AggregateData,
    },

    /// Represents a tree node which has not been modified, has an up-to-date
    /// hash, and which is being retained in memory.
    Loaded {
        /// Hash
        hash: CryptoHash,
        /// Child heights
        child_heights: (u8, u8),
        /// Tree
        tree: TreeNode,
        /// Aggregate data like Sum
        aggregate_data: AggregateData,
    },
}

#[cfg(feature = "minimal")]
impl Link {
    /// Creates a `Link::Modified` from the given `Tree`.
    #[inline]
    pub const fn from_modified_tree(tree: TreeNode) -> Self {
        let pending_writes = 1 + tree.child_pending_writes(true) + tree.child_pending_writes(false);

        Self::Modified {
            pending_writes,
            child_heights: tree.child_heights(),
            tree,
        }
    }

    /// Creates a `Link::Modified` from the given tree, if any. If `None`,
    /// returns `None`.
    pub fn maybe_from_modified_tree(maybe_tree: Option<TreeNode>) -> Option<Self> {
        maybe_tree.map(Self::from_modified_tree)
    }

    /// Returns `true` if the link is of the `Link::Reference` variant.
    #[inline]
    pub const fn is_reference(&self) -> bool {
        matches!(self, Link::Reference { .. })
    }

    /// Returns `true` if the link is of the `Link::Modified` variant.
    #[inline]
    pub const fn is_modified(&self) -> bool {
        matches!(self, Link::Modified { .. })
    }

    /// Returns `true` if the link is of the `Link::Uncommitted` variant.
    #[inline]
    pub const fn is_uncommitted(&self) -> bool {
        matches!(self, Link::Uncommitted { .. })
    }

    /// Returns `true` if the link is of the `Link::Loaded` variant.
    #[inline]
    pub const fn is_stored(&self) -> bool {
        matches!(self, Link::Loaded { .. })
    }

    /// Returns the key of the tree referenced by this link, as a slice.
    #[inline]
    pub fn key(&self) -> &[u8] {
        match self {
            Link::Reference { key, .. } => key.as_slice(),
            Link::Modified { tree, .. } => tree.key(),
            Link::Uncommitted { tree, .. } => tree.key(),
            Link::Loaded { tree, .. } => tree.key(),
        }
    }

    /// Returns the `Tree` instance of the tree referenced by the link. If the
    /// link is of variant `Link::Reference`, the returned value will be `None`.
    #[inline]
    pub const fn tree(&self) -> Option<&TreeNode> {
        match self {
            // TODO: panic for Reference, don't return Option?
            Link::Reference { .. } => None,
            Link::Modified { tree, .. } => Some(tree),
            Link::Uncommitted { tree, .. } => Some(tree),
            Link::Loaded { tree, .. } => Some(tree),
        }
    }

    /// Returns the hash of the tree referenced by the link. Panics if link is
    /// of variant `Link::Modified` since we have not yet recomputed the tree's
    /// hash.
    #[inline]
    pub const fn hash(&self) -> &CryptoHash {
        match self {
            Link::Modified { .. } => panic!("Cannot get hash from modified link"),
            Link::Reference { hash, .. } => hash,
            Link::Uncommitted { hash, .. } => hash,
            Link::Loaded { hash, .. } => hash,
        }
    }

    /// Returns the sum of the tree referenced by the link. Panics if link is
    /// of variant `Link::Modified` since we have not yet recomputed the tree's
    /// hash.
    #[inline]
    pub const fn aggregate_data(&self) -> AggregateData {
        match self {
            Link::Modified { .. } => panic!("Cannot get hash from modified link"),
            Link::Reference { aggregate_data, .. } => *aggregate_data,
            Link::Uncommitted { aggregate_data, .. } => *aggregate_data,
            Link::Loaded { aggregate_data, .. } => *aggregate_data,
        }
    }

    /// Returns the height of the children of the tree referenced by the link,
    /// if any (note: not the height of the referenced tree itself). Return
    /// value is `(left_child_height, right_child_height)`.
    #[inline]
    pub const fn height(&self) -> u8 {
        const fn max(a: u8, b: u8) -> u8 {
            if a >= b {
                a
            } else {
                b
            }
        }

        let (left_height, right_height) = match self {
            Link::Reference { child_heights, .. } => *child_heights,
            Link::Modified { child_heights, .. } => *child_heights,
            Link::Uncommitted { child_heights, .. } => *child_heights,
            Link::Loaded { child_heights, .. } => *child_heights,
        };
        1 + max(left_height, right_height)
    }

    /// Returns the balance factor of the tree referenced by the link.
    #[inline]
    pub const fn balance_factor(&self) -> i8 {
        let (left_height, right_height) = match self {
            Link::Reference { child_heights, .. } => *child_heights,
            Link::Modified { child_heights, .. } => *child_heights,
            Link::Uncommitted { child_heights, .. } => *child_heights,
            Link::Loaded { child_heights, .. } => *child_heights,
        };
        right_height as i8 - left_height as i8
    }

    /// Consumes the link and converts to variant `Link::Reference`. Panics if
    /// the link is of variant `Link::Modified` or `Link::Uncommitted`.
    #[inline]
    pub fn into_reference(self) -> Self {
        match self {
            Link::Reference { .. } => self,
            Link::Modified { .. } => panic!("Cannot prune Modified tree"),
            Link::Uncommitted { .. } => panic!("Cannot prune Uncommitted tree"),
            Link::Loaded {
                hash,
                aggregate_data,
                child_heights,
                tree,
            } => Self::Reference {
                hash,
                aggregate_data,
                child_heights,
                key: tree.take_key(),
            },
        }
    }

    #[inline]
    /// Return heights of children of the Link as mutable tuple
    pub(crate) fn child_heights_mut(&mut self) -> &mut (u8, u8) {
        match self {
            Link::Reference {
                ref mut child_heights,
                ..
            } => child_heights,
            Link::Modified {
                ref mut child_heights,
                ..
            } => child_heights,
            Link::Uncommitted {
                ref mut child_heights,
                ..
            } => child_heights,
            Link::Loaded {
                ref mut child_heights,
                ..
            } => child_heights,
        }
    }

    // Costs for operations within a single merk
    #[inline]
    /// Encoded link size
    pub const fn encoded_link_size(not_prefixed_key_len: u32, node_type: NodeType) -> u32 {
        let sum_tree_cost = node_type.cost();
        // Links are optional values that represent the right or left node for a given
        // 1 byte to represent key_length (this is a u8)
        // key_length to represent the actual key
        // 32 bytes for the hash of the node
        // 1 byte for the left child height
        // 1 byte for the right child height
        // 1 byte for the sum tree option
        not_prefixed_key_len + HASH_LENGTH_U32 + 4 + sum_tree_cost
    }

    /// The encoding cost is always 8 bytes for the sum instead of a varint
    #[inline]
    pub fn encoding_cost(&self) -> Result<usize> {
        debug_assert!(self.key().len() < 256, "Key length must be less than 256");

        Ok(match self {
            Link::Reference {
                key,
                aggregate_data,
                ..
            } => match aggregate_data {
                AggregateData::NoAggregateData => key.len() + 36, // 1 + HASH_LENGTH + 2 + 1,
                AggregateData::Count(_)
                | AggregateData::Sum(_)
                | AggregateData::ProvableCount(_) => {
                    // 1 for key len
                    // key_len for keys
                    // 32 for hash
                    // 2 for child heights
                    // 1 to represent presence of sum value
                    //    if above is 1, then
                    //    1 for sum len
                    //    sum_len for sum vale
                    key.len() + 44 // 1 + 32 + 2 + 1 + 8
                }
                AggregateData::BigSum(_)
                | AggregateData::CountAndSum(..)
                | AggregateData::ProvableCountAndSum(..) => {
                    // 1 for key len
                    // key_len for keys
                    // 32 for hash
                    // 2 for child heights
                    // 1 to represent presence of sum value
                    //    if above is 1, then
                    //    1 for sum len
                    //    sum_len for sum vale
                    key.len() + 52 // 1 + 32 + 2 + 1 + 16
                }
            },
            Link::Modified { .. } => panic!("No encoding for Link::Modified"),
            Link::Uncommitted {
                tree,
                aggregate_data,
                ..
            }
            | Link::Loaded {
                tree,
                aggregate_data,
                ..
            } => match aggregate_data {
                AggregateData::NoAggregateData => tree.key().len() + 36, // 1 + 32 + 2 + 1,
                AggregateData::Count(_)
                | AggregateData::Sum(_)
                | AggregateData::ProvableCount(_) => {
                    tree.key().len() + 44 // 1 + 32 + 2 + 1 + 8
                }
                AggregateData::BigSum(_)
                | AggregateData::CountAndSum(..)
                | AggregateData::ProvableCountAndSum(..) => {
                    tree.key().len() + 52 // 1 + 32 + 2 + 1 + 16
                }
            },
        })
    }
}

#[cfg(feature = "minimal")]
impl Encode for Link {
    #[inline]
    fn encode_into<W: Write>(&self, out: &mut W) -> Result<()> {
        let (hash, aggregate_data, key, (left_height, right_height)) = match self {
            Link::Reference {
                hash,
                aggregate_data,
                key,
                child_heights,
            } => (hash, aggregate_data, key.as_slice(), child_heights),
            Link::Loaded {
                hash,
                aggregate_data,
                tree,
                child_heights,
            } => (hash, aggregate_data, tree.key(), child_heights),
            Link::Uncommitted {
                hash,
                aggregate_data,
                tree,
                child_heights,
            } => (hash, aggregate_data, tree.key(), child_heights),

            Link::Modified { .. } => panic!("No encoding for Link::Modified"),
        };

        debug_assert!(key.len() < 256, "Key length must be less than 256");

        out.write_all(&[key.len() as u8])?;
        out.write_all(key)?;

        out.write_all(hash)?;

        out.write_all(&[*left_height, *right_height])?;

        match aggregate_data {
            AggregateData::NoAggregateData => {
                out.write_all(&[0])?;
            }
            AggregateData::Sum(sum_value) => {
                out.write_all(&[1])?;
                out.write_varint(*sum_value)?;
            }
            AggregateData::BigSum(big_sum_value) => {
                out.write_all(&[2])?;
                out.write_i128::<BigEndian>(*big_sum_value)?;
            }
            AggregateData::Count(count_value) => {
                out.write_all(&[3])?;
                out.write_varint(*count_value)?;
            }
            AggregateData::CountAndSum(count_value, sum_value) => {
                out.write_all(&[4])?;
                out.write_varint(*count_value)?;
                out.write_varint(*sum_value)?;
            }
            AggregateData::ProvableCount(count_value) => {
                out.write_all(&[5])?;
                out.write_varint(*count_value)?;
            }
            AggregateData::ProvableCountAndSum(count_value, sum_value) => {
                out.write_all(&[6])?;
                out.write_varint(*count_value)?;
                out.write_varint(*sum_value)?;
            }
        }

        Ok(())
    }

    #[inline]
    fn encoding_length(&self) -> Result<usize> {
        debug_assert!(self.key().len() < 256, "Key length must be less than 256");

        Ok(match self {
            Link::Reference {
                key,
                aggregate_data,
                ..
            } => match aggregate_data {
                AggregateData::NoAggregateData => key.len() + 36, // 1 + 32 + 2 + 1
                AggregateData::Sum(sum_value) => {
                    let encoded_sum_value = sum_value.encode_var_vec();
                    // 1 for key len
                    // key_len for keys
                    // 32 for hash
                    // 2 for child heights
                    // 1 to represent presence of sum value
                    //    if above is 1, then
                    //    1 for sum len
                    //    sum_len for sum vale
                    key.len() + encoded_sum_value.len() + 36 // 1 + 32 + 2 + 1
                }
                AggregateData::BigSum(_) => {
                    // 1 for key len
                    // key_len for keys
                    // 32 for hash
                    // 2 for child heights
                    // 1 to represent presence of sum value
                    //    if above is 1, then
                    //    1 for sum len
                    //    sum_len for sum vale
                    key.len() + 52 // 1 + 32 + 2 + 1 + 16
                }
                AggregateData::Count(count) => {
                    let encoded_count_value = count.encode_var_vec();
                    // 1 for key len
                    // key_len for keys
                    // 32 for hash
                    // 2 for child heights
                    // 1 to represent presence of sum value
                    //    if above is 1, then
                    //    1 for sum len
                    //    sum_len for sum vale
                    key.len() + encoded_count_value.len() + 36 // 1 + 32 + 2 + 1
                }
                AggregateData::CountAndSum(count, sum) => {
                    let encoded_sum_value = sum.encode_var_vec();
                    let encoded_count_value = count.encode_var_vec();
                    key.len() + encoded_sum_value.len() + encoded_count_value.len() + 36
                }
                AggregateData::ProvableCount(count) => {
                    let encoded_count_value = count.encode_var_vec();
                    key.len() + encoded_count_value.len() + 36
                }
                AggregateData::ProvableCountAndSum(count, sum) => {
                    let encoded_sum_value = sum.encode_var_vec();
                    let encoded_count_value = count.encode_var_vec();
                    key.len() + encoded_sum_value.len() + encoded_count_value.len() + 36
                }
            },
            Link::Modified { .. } => panic!("No encoding for Link::Modified"),
            Link::Uncommitted {
                tree,
                aggregate_data,
                ..
            }
            | Link::Loaded {
                tree,
                aggregate_data,
                ..
            } => match aggregate_data {
                AggregateData::NoAggregateData => tree.key().len() + 36, // 1 + 32 + 2 + 1
                AggregateData::Sum(sum_value) => {
                    let encoded_sum_value = sum_value.encode_var_vec();
                    tree.key().len() + encoded_sum_value.len() + 36 // 1 + 32 + 2 + 1
                }
                AggregateData::BigSum(_) => {
                    tree.key().len() + 52 // 1 + 32 + 2 + 1 + 16
                }
                AggregateData::Count(count_value) => {
                    let encoded_count_value = count_value.encode_var_vec();
                    tree.key().len() + encoded_count_value.len() + 36 // 1 + 32 + 2 + 1
                }
                AggregateData::CountAndSum(count, sum) => {
                    let encoded_sum_value = sum.encode_var_vec();
                    let encoded_count_value = count.encode_var_vec();
                    tree.key().len() + encoded_sum_value.len() + encoded_count_value.len() + 36
                }
                AggregateData::ProvableCount(count) => {
                    let encoded_count_value = count.encode_var_vec();
                    tree.key().len() + encoded_count_value.len() + 36
                }
                AggregateData::ProvableCountAndSum(count, sum) => {
                    let encoded_sum_value = sum.encode_var_vec();
                    let encoded_count_value = count.encode_var_vec();
                    tree.key().len() + encoded_sum_value.len() + encoded_count_value.len() + 36
                }
            },
        })
    }
}

#[cfg(feature = "minimal")]
impl Link {
    #[inline]
    fn default_reference() -> Self {
        Self::Reference {
            key: Vec::with_capacity(64),
            hash: Default::default(),
            aggregate_data: AggregateData::NoAggregateData,
            child_heights: (0, 0),
        }
    }
}

#[cfg(feature = "minimal")]
impl Decode for Link {
    #[inline]
    fn decode<R: Read>(input: R) -> Result<Self> {
        let mut link = Self::default_reference();
        Self::decode_into(&mut link, input)?;
        Ok(link)
    }

    #[inline]
    fn decode_into<R: Read>(&mut self, mut input: R) -> Result<()> {
        if !self.is_reference() {
            // don't create new struct if self is already Link::Reference,
            // so we can re-use the key vec
            *self = Self::default_reference();
        }

        if let Link::Reference {
            ref mut aggregate_data,
            ref mut key,
            ref mut hash,
            ref mut child_heights,
        } = self
        {
            let length = read_u8(&mut input)? as usize;

            key.resize(length, 0);
            input.read_exact(key.as_mut())?;

            input.read_exact(&mut hash[..])?;

            child_heights.0 = read_u8(&mut input)?;
            child_heights.1 = read_u8(&mut input)?;

            let aggregate_data_byte = read_u8(&mut input)?;
            *aggregate_data = match aggregate_data_byte {
                0 => AggregateData::NoAggregateData,
                1 => {
                    let encoded_sum: i64 = input.read_varint()?;
                    AggregateData::Sum(encoded_sum)
                }
                2 => {
                    let encoded_big_sum: i128 = input.read_i128::<BigEndian>()?;
                    AggregateData::BigSum(encoded_big_sum)
                }
                3 => {
                    let encoded_count: u64 = input.read_varint()?;
                    AggregateData::Count(encoded_count)
                }
                4 => {
                    let encoded_count: u64 = input.read_varint()?;
                    let encoded_sum: i64 = input.read_varint()?;
                    AggregateData::CountAndSum(encoded_count, encoded_sum)
                }
                5 => {
                    let encoded_count: u64 = input.read_varint()?;
                    AggregateData::ProvableCount(encoded_count)
                }
                6 => {
                    let encoded_count: u64 = input.read_varint()?;
                    let encoded_sum: i64 = input.read_varint()?;
                    AggregateData::ProvableCountAndSum(encoded_count, encoded_sum)
                }
                byte => return Err(ed::Error::UnexpectedByte(byte)),
            };
        } else {
            unreachable!()
        }

        Ok(())
    }
}

#[cfg(feature = "minimal")]
impl Terminated for Link {}

#[cfg(feature = "minimal")]
#[inline]
fn read_u8<R: Read>(mut input: R) -> Result<u8> {
    let mut length = [0];
    input.read_exact(length.as_mut())?;
    Ok(length[0])
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod test {
    use super::{
        super::{hash::NULL_HASH, TreeNode},
        *,
    };
    use crate::TreeFeatureType::BasicMerkNode;

    #[test]
    fn from_modified_tree() {
        let tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap();
        let link = Link::from_modified_tree(tree);
        assert!(link.is_modified());
        assert_eq!(link.height(), 1);
        assert_eq!(link.tree().expect("expected tree").key(), &[0]);
        if let Link::Modified { pending_writes, .. } = link {
            assert_eq!(pending_writes, 1);
        } else {
            panic!("Expected Link::Modified");
        }
    }

    #[test]
    fn maybe_from_modified_tree() {
        let link = Link::maybe_from_modified_tree(None);
        assert!(link.is_none());

        let tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap();
        let link = Link::maybe_from_modified_tree(Some(tree));
        assert!(link.expect("expected link").is_modified());
    }

    #[test]
    fn types() {
        let hash = NULL_HASH;
        let aggregate_data = AggregateData::NoAggregateData;
        let child_heights = (0, 0);
        let pending_writes = 1;
        let key = vec![0];
        let tree = || TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap();

        let reference = Link::Reference {
            hash,
            aggregate_data,
            child_heights,
            key,
        };
        let modified = Link::Modified {
            pending_writes,
            child_heights,
            tree: tree(),
        };
        let uncommitted = Link::Uncommitted {
            hash,
            aggregate_data,
            child_heights,
            tree: tree(),
        };
        let loaded = Link::Loaded {
            hash,
            aggregate_data,
            child_heights,
            tree: tree(),
        };

        assert!(reference.is_reference());
        assert!(!reference.is_modified());
        assert!(!reference.is_uncommitted());
        assert!(!reference.is_stored());
        assert!(reference.tree().is_none());
        assert_eq!(reference.hash(), &[0; 32]);
        assert_eq!(reference.height(), 1);
        assert!(reference.into_reference().is_reference());

        assert!(!modified.is_reference());
        assert!(modified.is_modified());
        assert!(!modified.is_uncommitted());
        assert!(!modified.is_stored());
        assert!(modified.tree().is_some());
        assert_eq!(modified.height(), 1);

        assert!(!uncommitted.is_reference());
        assert!(!uncommitted.is_modified());
        assert!(uncommitted.is_uncommitted());
        assert!(!uncommitted.is_stored());
        assert!(uncommitted.tree().is_some());
        assert_eq!(uncommitted.hash(), &[0; 32]);
        assert_eq!(uncommitted.height(), 1);

        assert!(!loaded.is_reference());
        assert!(!loaded.is_modified());
        assert!(!loaded.is_uncommitted());
        assert!(loaded.is_stored());
        assert!(loaded.tree().is_some());
        assert_eq!(loaded.hash(), &[0; 32]);
        assert_eq!(loaded.height(), 1);
        assert!(loaded.into_reference().is_reference());
    }

    #[test]
    #[should_panic]
    fn modified_hash() {
        Link::Modified {
            pending_writes: 1,
            child_heights: (1, 1),
            tree: TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap(),
        }
        .hash();
    }

    #[test]
    #[should_panic]
    fn modified_into_reference() {
        Link::Modified {
            pending_writes: 1,
            child_heights: (1, 1),
            tree: TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap(),
        }
        .into_reference();
    }

    #[test]
    #[should_panic]
    fn uncommitted_into_reference() {
        Link::Uncommitted {
            hash: [1; 32],
            aggregate_data: AggregateData::NoAggregateData,
            child_heights: (1, 1),
            tree: TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap(),
        }
        .into_reference();
    }

    #[test]
    fn encode_link() {
        let link = Link::Reference {
            key: vec![1, 2, 3],
            aggregate_data: AggregateData::NoAggregateData,
            child_heights: (123, 124),
            hash: [55; 32],
        };
        assert_eq!(link.encoding_length().unwrap(), 39);

        let mut bytes = vec![];
        link.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                3, 1, 2, 3, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 123, 124, 0
            ]
        );
    }

    #[test]
    fn encode_link_with_sum() {
        let link = Link::Reference {
            key: vec![1, 2, 3],
            aggregate_data: AggregateData::Sum(50),
            child_heights: (123, 124),
            hash: [55; 32],
        };
        assert_eq!(link.encoding_length().unwrap(), 40);

        let mut bytes = vec![];
        link.encode_into(&mut bytes).unwrap();

        assert_eq!(link.encoding_length().unwrap(), bytes.len());
        assert_eq!(
            bytes,
            vec![
                3, 1, 2, 3, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 123, 124, 1, 100,
            ]
        );
    }

    #[test]
    fn encode_link_with_count() {
        let link = Link::Reference {
            key: vec![1, 2, 3],
            aggregate_data: AggregateData::Count(50),
            child_heights: (123, 124),
            hash: [55; 32],
        };
        assert_eq!(link.encoding_length().unwrap(), 40);

        let mut bytes = vec![];
        link.encode_into(&mut bytes).unwrap();

        assert_eq!(link.encoding_length().unwrap(), bytes.len());
        assert_eq!(
            bytes,
            vec![
                3, 1, 2, 3, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 123, 124, 3, 50,
            ]
        );
    }

    #[test]
    fn encode_link_with_big_sum() {
        let link = Link::Reference {
            key: vec![1, 2, 3],
            aggregate_data: AggregateData::BigSum(50),
            child_heights: (123, 124),
            hash: [55; 32],
        };
        assert_eq!(link.encoding_length().unwrap(), 55);

        let mut bytes = vec![];
        link.encode_into(&mut bytes).unwrap();

        assert_eq!(link.encoding_length().unwrap(), bytes.len());
        assert_eq!(
            bytes,
            vec![
                3, 1, 2, 3, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 123, 124, 2, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50
            ]
        );
    }

    #[test]
    #[should_panic]
    fn encode_link_long_key() {
        let link = Link::Reference {
            key: vec![123; 300],
            aggregate_data: AggregateData::NoAggregateData,
            child_heights: (123, 124),
            hash: [55; 32],
        };
        let mut bytes = vec![];
        link.encode_into(&mut bytes).unwrap();
    }

    #[test]
    fn decode_link() {
        let bytes = vec![
            3, 1, 2, 3, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
            55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 123, 124, 0,
        ];
        let link = Link::decode(bytes.as_slice()).expect("expected to decode a link");
        assert_eq!(link.aggregate_data(), AggregateData::NoAggregateData);
    }
}
