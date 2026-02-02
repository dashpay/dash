//! Merk tree feature type

#[cfg(any(feature = "minimal", feature = "verify"))]
use std::io::{Read, Write};

#[cfg(any(feature = "minimal", feature = "verify"))]
use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
#[cfg(feature = "minimal")]
use ed::Terminated;
#[cfg(any(feature = "minimal", feature = "verify"))]
use ed::{Decode, Encode};
#[cfg(any(feature = "minimal", feature = "verify"))]
use grovedb_costs::TreeCostType;
#[cfg(any(feature = "minimal", feature = "verify"))]
use integer_encoding::{VarInt, VarIntReader, VarIntWriter};

#[cfg(feature = "minimal")]
use crate::merk::NodeType;
#[cfg(any(feature = "minimal", feature = "verify"))]
use crate::tree::tree_feature_type::TreeFeatureType::{
    BasicMerkNode, BigSummedMerkNode, CountedMerkNode, CountedSummedMerkNode, SummedMerkNode,
};
#[cfg(feature = "minimal")]
use crate::tree_type::TreeType;
use crate::TreeFeatureType::ProvableCountedMerkNode;

#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
/// Basic or summed
pub enum TreeFeatureType {
    /// Basic Merk Tree Node
    BasicMerkNode,
    /// Summed Merk Tree Node
    SummedMerkNode(i64),
    /// Big Summed Merk Tree Node
    BigSummedMerkNode(i128),
    /// Counted Merk Tree None
    CountedMerkNode(u64),
    /// Counted and summed Merk Tree None
    CountedSummedMerkNode(u64, i64),
    /// Provable Counted Merk Tree Node
    ProvableCountedMerkNode(u64),
    /// Provable Counted and Summed Merk Tree Node (count in hash, sum tracked)
    ProvableCountedSummedMerkNode(u64, i64),
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl TreeFeatureType {
    /// Returns the count of elements in this subtree, if available.
    /// Returns Some(count) for CountedMerkNode, ProvableCountedMerkNode,
    /// CountedSummedMerkNode, and ProvableCountedSummedMerkNode variants.
    /// Returns None for BasicMerkNode, SummedMerkNode, BigSummedMerkNode.
    pub fn count(&self) -> Option<u64> {
        match self {
            CountedMerkNode(count)
            | ProvableCountedMerkNode(count)
            | CountedSummedMerkNode(count, _)
            | TreeFeatureType::ProvableCountedSummedMerkNode(count, _) => Some(*count),
            BasicMerkNode | SummedMerkNode(_) | BigSummedMerkNode(_) => None,
        }
    }
}

#[cfg(feature = "minimal")]
impl TreeFeatureType {
    pub fn node_type(&self) -> NodeType {
        match self {
            BasicMerkNode => NodeType::NormalNode,
            SummedMerkNode(_) => NodeType::SumNode,
            BigSummedMerkNode(_) => NodeType::BigSumNode,
            CountedMerkNode(_) => NodeType::CountNode,
            CountedSummedMerkNode(..) => NodeType::CountSumNode,
            ProvableCountedMerkNode(_) => NodeType::ProvableCountNode,
            TreeFeatureType::ProvableCountedSummedMerkNode(..) => NodeType::ProvableCountSumNode,
        }
    }
}

#[cfg(feature = "minimal")]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AggregateData {
    NoAggregateData,
    Sum(i64),
    BigSum(i128),
    Count(u64),
    CountAndSum(u64, i64),
    ProvableCount(u64),
    ProvableCountAndSum(u64, i64),
}

#[cfg(feature = "minimal")]
impl AggregateData {
    pub fn parent_tree_type(&self) -> TreeType {
        match self {
            AggregateData::NoAggregateData => TreeType::NormalTree,
            AggregateData::Sum(_) => TreeType::SumTree,
            AggregateData::BigSum(_) => TreeType::BigSumTree,
            AggregateData::Count(_) => TreeType::CountTree,
            AggregateData::CountAndSum(..) => TreeType::CountSumTree,
            AggregateData::ProvableCount(_) => TreeType::ProvableCountTree,
            AggregateData::ProvableCountAndSum(..) => TreeType::ProvableCountSumTree,
        }
    }

    pub fn as_sum_i64(&self) -> i64 {
        match self {
            AggregateData::NoAggregateData => 0,
            AggregateData::Sum(s) => *s,
            AggregateData::BigSum(i) => {
                let max = i64::MAX as i128;
                if *i > max {
                    i64::MAX
                } else {
                    *i as i64
                }
            }
            AggregateData::Count(_) => 0,
            AggregateData::CountAndSum(_, s) => *s,
            AggregateData::ProvableCount(_) => 0,
            AggregateData::ProvableCountAndSum(_, s) => *s,
        }
    }

    pub fn as_count_u64(&self) -> u64 {
        match self {
            AggregateData::NoAggregateData => 0,
            AggregateData::Sum(_) => 0,
            AggregateData::BigSum(_) => 0,
            AggregateData::Count(c) => *c,
            AggregateData::CountAndSum(c, _) => *c,
            AggregateData::ProvableCount(c) => *c,
            AggregateData::ProvableCountAndSum(c, _) => *c,
        }
    }

    pub fn as_summed_i128(&self) -> i128 {
        match self {
            AggregateData::NoAggregateData => 0,
            AggregateData::Sum(s) => *s as i128,
            AggregateData::BigSum(i) => *i,
            AggregateData::Count(_) => 0,
            AggregateData::CountAndSum(_, s) => *s as i128,
            AggregateData::ProvableCount(_) => 0,
            AggregateData::ProvableCountAndSum(_, s) => *s as i128,
        }
    }
}

#[cfg(feature = "minimal")]
impl From<TreeFeatureType> for AggregateData {
    fn from(value: TreeFeatureType) -> Self {
        match value {
            BasicMerkNode => AggregateData::NoAggregateData,
            SummedMerkNode(val) => AggregateData::Sum(val),
            BigSummedMerkNode(val) => AggregateData::BigSum(val),
            CountedMerkNode(val) => AggregateData::Count(val),
            CountedSummedMerkNode(count, sum) => AggregateData::CountAndSum(count, sum),
            TreeFeatureType::ProvableCountedMerkNode(val) => AggregateData::ProvableCount(val),
            TreeFeatureType::ProvableCountedSummedMerkNode(count, sum) => {
                AggregateData::ProvableCountAndSum(count, sum)
            }
        }
    }
}

#[cfg(feature = "minimal")]
impl TreeFeatureType {
    #[inline]
    /// Get length of encoded SummedMerk
    pub fn tree_feature_specialized_type_and_length(&self) -> Option<(TreeCostType, u32)> {
        match self {
            BasicMerkNode => None,
            SummedMerkNode(m) => Some((
                TreeCostType::TreeFeatureUsesVarIntCostAs8Bytes,
                m.encode_var_vec().len() as u32,
            )),
            BigSummedMerkNode(_) => Some((TreeCostType::TreeFeatureUses16Bytes, 16)),
            CountedMerkNode(m) => Some((
                TreeCostType::TreeFeatureUsesVarIntCostAs8Bytes,
                m.encode_var_vec().len() as u32,
            )),
            CountedSummedMerkNode(count, sum) => Some((
                TreeCostType::TreeFeatureUsesTwoVarIntsCostAs16Bytes,
                count.encode_var_vec().len() as u32 + sum.encode_var_vec().len() as u32,
            )),
            TreeFeatureType::ProvableCountedMerkNode(m) => Some((
                TreeCostType::TreeFeatureUsesVarIntCostAs8Bytes,
                m.encode_var_vec().len() as u32,
            )),
            TreeFeatureType::ProvableCountedSummedMerkNode(count, sum) => Some((
                TreeCostType::TreeFeatureUsesTwoVarIntsCostAs16Bytes,
                count.encode_var_vec().len() as u32 + sum.encode_var_vec().len() as u32,
            )),
        }
    }

    #[inline]
    /// Get encoding cost of self
    pub(crate) fn encoding_cost(&self) -> usize {
        match self {
            BasicMerkNode => 1,
            SummedMerkNode(_sum) => 9,
            BigSummedMerkNode(_) => 17,
            CountedMerkNode(_) => 9,
            CountedSummedMerkNode(..) => 17,
            TreeFeatureType::ProvableCountedMerkNode(_) => 9,
            TreeFeatureType::ProvableCountedSummedMerkNode(..) => 17,
        }
    }
}

#[cfg(feature = "minimal")]
impl Terminated for TreeFeatureType {}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Encode for TreeFeatureType {
    #[inline]
    fn encode_into<W: Write>(&self, dest: &mut W) -> ed::Result<()> {
        match self {
            BasicMerkNode => {
                dest.write_all(&[0])?;
                Ok(())
            }
            SummedMerkNode(sum) => {
                dest.write_all(&[1])?;
                dest.write_varint(*sum)?;
                Ok(())
            }
            BigSummedMerkNode(sum) => {
                dest.write_all(&[2])?;
                dest.write_i128::<BigEndian>(*sum)?;
                Ok(())
            }
            CountedMerkNode(count) => {
                dest.write_all(&[3])?;
                dest.write_varint(*count)?;
                Ok(())
            }
            CountedSummedMerkNode(count, sum) => {
                dest.write_all(&[4])?;
                dest.write_varint(*count)?;
                dest.write_varint(*sum)?;
                Ok(())
            }
            TreeFeatureType::ProvableCountedMerkNode(count) => {
                dest.write_all(&[5])?;
                dest.write_varint(*count)?;
                Ok(())
            }
            TreeFeatureType::ProvableCountedSummedMerkNode(count, sum) => {
                dest.write_all(&[6])?;
                dest.write_varint(*count)?;
                dest.write_varint(*sum)?;
                Ok(())
            }
        }
    }

    #[inline]
    fn encoding_length(&self) -> ed::Result<usize> {
        match self {
            BasicMerkNode => Ok(1),
            SummedMerkNode(sum) => {
                let encoded_sum = sum.encode_var_vec();
                // 1 for the enum type
                // encoded_sum.len() for the length of the encoded vector
                Ok(1 + encoded_sum.len())
            }
            BigSummedMerkNode(_) => Ok(17),
            CountedMerkNode(count) => {
                let encoded_sum = count.encode_var_vec();
                // 1 for the enum type
                // encoded_sum.len() for the length of the encoded vector
                Ok(1 + encoded_sum.len())
            }
            CountedSummedMerkNode(count, sum) => {
                let encoded_lengths = count.encode_var_vec().len() + sum.encode_var_vec().len();
                // 1 for the enum type
                Ok(1 + encoded_lengths)
            }
            TreeFeatureType::ProvableCountedMerkNode(count) => {
                let encoded_sum = count.encode_var_vec();
                // 1 for the enum type
                // encoded_sum.len() for the length of the encoded vector
                Ok(1 + encoded_sum.len())
            }
            TreeFeatureType::ProvableCountedSummedMerkNode(count, sum) => {
                let encoded_lengths = count.encode_var_vec().len() + sum.encode_var_vec().len();
                // 1 for the enum type
                Ok(1 + encoded_lengths)
            }
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Decode for TreeFeatureType {
    #[inline]
    fn decode<R: Read>(mut input: R) -> ed::Result<Self> {
        let mut feature_type: [u8; 1] = [0];
        input.read_exact(&mut feature_type)?;
        match feature_type {
            [0] => Ok(BasicMerkNode),
            [1] => {
                let encoded_sum: i64 = input.read_varint()?;
                Ok(SummedMerkNode(encoded_sum))
            }
            [2] => {
                let encoded_sum: i128 = input.read_i128::<BigEndian>()?;
                Ok(BigSummedMerkNode(encoded_sum))
            }
            [3] => {
                let encoded_count: u64 = input.read_varint()?;
                Ok(CountedMerkNode(encoded_count))
            }
            [4] => {
                let encoded_count: u64 = input.read_varint()?;
                let encoded_sum: i64 = input.read_varint()?;
                Ok(CountedSummedMerkNode(encoded_count, encoded_sum))
            }
            [5] => {
                let encoded_count: u64 = input.read_varint()?;
                Ok(ProvableCountedMerkNode(encoded_count))
            }
            [6] => {
                let encoded_count: u64 = input.read_varint()?;
                let encoded_sum: i64 = input.read_varint()?;
                Ok(TreeFeatureType::ProvableCountedSummedMerkNode(
                    encoded_count,
                    encoded_sum,
                ))
            }
            _ => Err(ed::Error::UnexpectedByte(55)),
        }
    }
}
