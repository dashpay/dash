//! Merk tree encoding

#[cfg(feature = "minimal")]
use ed::{Decode, Encode};
#[cfg(feature = "minimal")]
use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_no_add, CostResult, CostsExt, OperationCost,
};
#[cfg(feature = "minimal")]
use grovedb_storage::StorageContext;
use grovedb_version::version::GroveVersion;

#[cfg(feature = "minimal")]
use super::TreeNode;
use crate::tree::kv::ValueDefinedCostType;
#[cfg(feature = "minimal")]
use crate::{
    error::{Error, Error::EdError},
    tree::TreeNodeInner,
    Error::StorageError,
};

#[cfg(feature = "minimal")]
impl TreeNode {
    /// Decode given bytes and set as Tree fields. Set key to value of given
    /// key.
    pub fn decode_raw(
        bytes: &[u8],
        key: Vec<u8>,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> Result<Self, Error> {
        TreeNode::decode(key, bytes, value_defined_cost_fn, grove_version).map_err(EdError)
    }

    /// Get value from storage given key.
    pub(crate) fn get<'db, S, K>(
        storage: &S,
        key: K,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Self>, Error>
    where
        S: StorageContext<'db>,
        K: AsRef<[u8]>,
    {
        let mut cost = OperationCost::default();
        let tree_bytes = cost_return_on_error!(&mut cost, storage.get(&key).map_err(StorageError));

        let tree_opt = cost_return_on_error_no_add!(
            cost,
            tree_bytes
                .map(|x| TreeNode::decode_raw(
                    &x,
                    key.as_ref().to_vec(),
                    value_defined_cost_fn,
                    grove_version
                ))
                .transpose()
        );

        Ok(tree_opt).wrap_with_cost(cost)
    }
}

#[cfg(feature = "minimal")]
impl TreeNode {
    #[inline]
    /// Encode
    pub fn encode(&self) -> Vec<u8> {
        // operation is infallible so it's ok to unwrap
        Encode::encode(&self.inner).unwrap()
    }

    #[inline]
    /// Encode to destination writer
    pub fn encode_into(&self, dest: &mut Vec<u8>) {
        // operation is infallible so it's ok to unwrap
        Encode::encode_into(&self.inner, dest).unwrap()
    }

    #[inline]
    /// Return length of encoding
    pub fn encoding_length(&self) -> usize {
        // operation is infallible so it's ok to unwrap
        Encode::encoding_length(&self.inner).unwrap()
    }

    #[inline]
    /// Get the cost (byte length) of the value including parent to child
    /// reference (or hook)
    pub fn value_encoding_length_with_parent_to_child_reference(&self) -> u32 {
        // in the case of a grovedb tree the value cost is fixed
        if let Some(value_cost) = &self.inner.kv.value_defined_cost {
            self.inner.kv.predefined_value_byte_cost_size(value_cost)
        } else {
            self.inner.kv.value_byte_cost_size()
        }
    }

    #[inline]
    /// Decode bytes from reader, set as Tree fields and set key to given key
    pub fn decode_into(
        &mut self,
        key: Vec<u8>,
        input: &[u8],
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> ed::Result<()> {
        let mut tree_inner: TreeNodeInner = Decode::decode(input)?;
        tree_inner.kv.key = key;
        if let Some(value_defined_cost_fn) = value_defined_cost_fn {
            tree_inner.kv.value_defined_cost =
                value_defined_cost_fn(tree_inner.kv.value.as_slice(), grove_version);
        }
        self.inner = Box::new(tree_inner);
        Ok(())
    }

    #[inline]
    /// Decode input and set as Tree fields. Set the key as the given key.
    pub fn decode(
        key: Vec<u8>,
        input: &[u8],
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> ed::Result<Self> {
        let mut tree_inner: TreeNodeInner = Decode::decode(input)?;
        tree_inner.kv.key = key;
        if let Some(value_defined_cost_fn) = value_defined_cost_fn {
            tree_inner.kv.value_defined_cost =
                value_defined_cost_fn(tree_inner.kv.value.as_slice(), grove_version);
        }
        Ok(TreeNode::new_with_tree_inner(tree_inner))
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod tests {
    use super::{super::Link, *};
    use crate::{
        tree::AggregateData,
        TreeFeatureType::{BasicMerkNode, SummedMerkNode},
    };

    #[test]
    fn encode_leaf_tree() {
        let tree =
            TreeNode::from_fields(vec![0], vec![1], [55; 32], None, None, BasicMerkNode).unwrap();
        assert_eq!(tree.encoding_length(), 68);
        assert_eq!(
            tree.value_encoding_length_with_parent_to_child_reference(),
            104
        );
        assert_eq!(
            tree.encode(),
            vec![
                0, 0, 0, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 32, 34, 236, 157, 87, 27,
                167, 116, 207, 158, 131, 208, 25, 73, 98, 245, 209, 227, 170, 26, 72, 212, 134,
                166, 126, 39, 98, 166, 199, 149, 144, 21, 1
            ]
        );
    }

    #[test]
    #[should_panic]
    fn encode_modified_tree() {
        let tree = TreeNode::from_fields(
            vec![0],
            vec![1],
            [55; 32],
            Some(Link::Modified {
                pending_writes: 1,
                child_heights: (123, 124),
                tree: TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap(),
            }),
            None,
            BasicMerkNode,
        )
        .unwrap();
        tree.encode();
    }

    #[test]
    fn encode_loaded_tree() {
        let tree = TreeNode::from_fields(
            vec![0],
            vec![1],
            [55; 32],
            Some(Link::Loaded {
                hash: [66; 32],
                aggregate_data: AggregateData::NoAggregateData,
                child_heights: (123, 124),
                tree: TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap(),
            }),
            None,
            BasicMerkNode,
        )
        .unwrap();
        assert_eq!(
            tree.encode(),
            vec![
                1, 1, 2, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
                66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 123, 124, 0, 0, 0, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 32, 34, 236, 157, 87, 27, 167, 116, 207, 158,
                131, 208, 25, 73, 98, 245, 209, 227, 170, 26, 72, 212, 134, 166, 126, 39, 98, 166,
                199, 149, 144, 21, 1
            ]
        );
    }

    #[test]
    fn encode_uncommitted_tree() {
        let tree = TreeNode::from_fields(
            vec![0],
            vec![1],
            [55; 32],
            Some(Link::Uncommitted {
                hash: [66; 32],
                aggregate_data: AggregateData::Sum(10),
                child_heights: (123, 124),
                tree: TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap(),
            }),
            None,
            SummedMerkNode(5),
        )
        .unwrap();
        assert_eq!(
            tree.encode(),
            vec![
                1, 1, 2, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
                66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 123, 124, 1, 20, 0, 1, 10,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 32, 34, 236, 157, 87, 27, 167, 116,
                207, 158, 131, 208, 25, 73, 98, 245, 209, 227, 170, 26, 72, 212, 134, 166, 126, 39,
                98, 166, 199, 149, 144, 21, 1
            ]
        );
    }

    #[test]
    fn encode_reference_tree() {
        let tree = TreeNode::from_fields(
            vec![0],
            vec![1],
            [55; 32],
            Some(Link::Reference {
                hash: [66; 32],
                aggregate_data: AggregateData::NoAggregateData,
                child_heights: (123, 124),
                key: vec![2],
            }),
            None,
            BasicMerkNode,
        )
        .unwrap();
        assert_eq!(
            tree.encoding_length(), /* this does not have the key encoded, just value and
                                     * left/right */
            105
        );
        assert_eq!(
            tree.value_encoding_length_with_parent_to_child_reference(),
            104 // This is 1 less, because the right "Option" byte was not paid for
        );
        assert_eq!(
            tree.encode(),
            vec![
                1, 1, 2, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
                66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 123, 124, 0, 0, 0, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                55, 55, 55, 55, 55, 55, 55, 55, 55, 32, 34, 236, 157, 87, 27, 167, 116, 207, 158,
                131, 208, 25, 73, 98, 245, 209, 227, 170, 26, 72, 212, 134, 166, 126, 39, 98, 166,
                199, 149, 144, 21, 1
            ]
        );
    }

    #[test]
    fn decode_leaf_tree() {
        let grove_version = GroveVersion::latest();
        let bytes = vec![
            0, 0, 0, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
            55, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 32, 34, 236, 157, 87, 27, 167, 116, 207, 158,
            131, 208, 25, 73, 98, 245, 209, 227, 170, 26, 72, 212, 134, 166, 126, 39, 98, 166, 199,
            149, 144, 21, 1,
        ];
        let tree = TreeNode::decode(
            vec![0],
            bytes.as_slice(),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .expect("should decode correctly");
        assert_eq!(tree.key(), &[0]);
        assert_eq!(tree.value_as_slice(), &[1]);
        assert_eq!(tree.inner.kv.feature_type, BasicMerkNode);
    }

    #[test]
    fn decode_reference_tree() {
        let grove_version = GroveVersion::latest();
        let bytes = vec![
            1, 1, 2, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
            66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 123, 124, 0, 0, 0, 55, 55, 55, 55,
            55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
            55, 55, 55, 55, 55, 55, 32, 34, 236, 157, 87, 27, 167, 116, 207, 158, 131, 208, 25, 73,
            98, 245, 209, 227, 170, 26, 72, 212, 134, 166, 126, 39, 98, 166, 199, 149, 144, 21, 1,
        ];
        let tree = TreeNode::decode(
            vec![0],
            bytes.as_slice(),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .expect("should decode correctly");
        assert_eq!(tree.key(), &[0]);
        assert_eq!(tree.value_as_slice(), &[1]);
        if let Some(Link::Reference {
            key,
            child_heights,
            hash,
            aggregate_data: _,
        }) = tree.link(true)
        {
            assert_eq!(*key, [2]);
            assert_eq!(*child_heights, (123u8, 124u8));
            assert_eq!(*hash, [66u8; 32]);
        } else {
            panic!("Expected Link::Reference");
        }
    }

    #[test]
    fn decode_invalid_bytes_as_tree() {
        let grove_version = GroveVersion::latest();
        let bytes = vec![2, 3, 4, 5];
        let tree = TreeNode::decode(
            vec![0],
            bytes.as_slice(),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        );
        assert!(tree.is_err());
    }
}
