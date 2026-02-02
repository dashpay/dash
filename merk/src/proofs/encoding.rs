//! Proofs encoding

#[cfg(any(feature = "minimal", feature = "verify"))]
use std::io::{Read, Write};

#[cfg(feature = "minimal")]
use ed::Terminated;
#[cfg(any(feature = "minimal", feature = "verify"))]
use ed::{Decode, Encode, Error as EdError};

#[cfg(any(feature = "minimal", feature = "verify"))]
use super::{Node, Op};
#[cfg(any(feature = "minimal", feature = "verify"))]
use crate::{error::Error, tree::HASH_LENGTH, TreeFeatureType};

// Large value opcodes use u32 for value length (values >= 64KB)
// Push large variants: 0x20-0x25
// PushInverted large variants: 0x28-0x2d

/// Maximum allowed value length for large value variants (64MB).
/// This prevents DoS attacks via malicious proofs specifying unreasonably large
/// allocations.
#[cfg(any(feature = "minimal", feature = "verify"))]
const MAX_VALUE_LEN: u32 = 64 * 1024 * 1024;

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Encode for Op {
    fn encode_into<W: Write>(&self, dest: &mut W) -> ed::Result<()> {
        match self {
            // Push
            Op::Push(Node::Hash(hash)) => {
                dest.write_all(&[0x01])?;
                dest.write_all(hash)?;
            }
            Op::Push(Node::KVHash(kv_hash)) => {
                dest.write_all(&[0x02])?;
                dest.write_all(kv_hash)?;
            }
            Op::Push(Node::KV(key, value)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x03, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                } else {
                    dest.write_all(&[0x20, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                }
            }
            Op::Push(Node::KVValueHash(key, value, value_hash)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x04, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                } else {
                    dest.write_all(&[0x21, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                }
            }
            Op::Push(Node::KVDigest(key, value_hash)) => {
                debug_assert!(key.len() < 256);

                dest.write_all(&[0x05, key.len() as u8])?;
                dest.write_all(key)?;
                dest.write_all(value_hash)?;
            }
            Op::Push(Node::KVRefValueHash(key, value, value_hash)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x06, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                } else {
                    dest.write_all(&[0x22, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                }
            }
            Op::Push(Node::KVValueHashFeatureType(key, value, value_hash, feature_type)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x07, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                    feature_type.encode_into(dest)?;
                } else {
                    dest.write_all(&[0x23, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                    feature_type.encode_into(dest)?;
                }
            }
            Op::Push(Node::KVCount(key, value, count)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x14, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    count.encode_into(dest)?;
                } else {
                    dest.write_all(&[0x24, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    count.encode_into(dest)?;
                }
            }
            Op::Push(Node::KVHashCount(kv_hash, count)) => {
                dest.write_all(&[0x15])?;
                dest.write_all(kv_hash)?;
                count.encode_into(dest)?;
            }
            Op::Push(Node::KVRefValueHashCount(key, value, value_hash, count)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x18, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                    count.encode_into(dest)?;
                } else {
                    dest.write_all(&[0x25, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                    count.encode_into(dest)?;
                }
            }
            Op::Push(Node::KVDigestCount(key, value_hash, count)) => {
                debug_assert!(key.len() < 256);

                dest.write_all(&[0x1a, key.len() as u8])?;
                dest.write_all(key)?;
                dest.write_all(value_hash)?;
                count.encode_into(dest)?;
            }

            // PushInverted
            Op::PushInverted(Node::Hash(hash)) => {
                dest.write_all(&[0x08])?;
                dest.write_all(hash)?;
            }
            Op::PushInverted(Node::KVHash(kv_hash)) => {
                dest.write_all(&[0x09])?;
                dest.write_all(kv_hash)?;
            }
            Op::PushInverted(Node::KV(key, value)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x0a, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                } else {
                    dest.write_all(&[0x28, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                }
            }
            Op::PushInverted(Node::KVValueHash(key, value, value_hash)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x0b, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                } else {
                    dest.write_all(&[0x29, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                }
            }
            Op::PushInverted(Node::KVDigest(key, value_hash)) => {
                debug_assert!(key.len() < 256);

                dest.write_all(&[0x0c, key.len() as u8])?;
                dest.write_all(key)?;
                dest.write_all(value_hash)?;
            }
            Op::PushInverted(Node::KVRefValueHash(key, value, value_hash)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x0d, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                } else {
                    dest.write_all(&[0x2a, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                }
            }
            Op::PushInverted(Node::KVValueHashFeatureType(
                key,
                value,
                value_hash,
                feature_type,
            )) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x0e, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                    feature_type.encode_into(dest)?;
                } else {
                    dest.write_all(&[0x2b, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                    feature_type.encode_into(dest)?;
                }
            }
            Op::PushInverted(Node::KVCount(key, value, count)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x16, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    count.encode_into(dest)?;
                } else {
                    dest.write_all(&[0x2c, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    count.encode_into(dest)?;
                }
            }
            Op::PushInverted(Node::KVHashCount(kv_hash, count)) => {
                dest.write_all(&[0x17])?;
                dest.write_all(kv_hash)?;
                count.encode_into(dest)?;
            }
            Op::PushInverted(Node::KVRefValueHashCount(key, value, value_hash, count)) => {
                debug_assert!(key.len() < 256);
                if value.len() < 65536 {
                    dest.write_all(&[0x19, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u16).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                    count.encode_into(dest)?;
                } else {
                    dest.write_all(&[0x2d, key.len() as u8])?;
                    dest.write_all(key)?;
                    (value.len() as u32).encode_into(dest)?;
                    dest.write_all(value)?;
                    dest.write_all(value_hash)?;
                    count.encode_into(dest)?;
                }
            }
            Op::PushInverted(Node::KVDigestCount(key, value_hash, count)) => {
                debug_assert!(key.len() < 256);

                dest.write_all(&[0x1b, key.len() as u8])?;
                dest.write_all(key)?;
                dest.write_all(value_hash)?;
                count.encode_into(dest)?;
            }

            Op::Parent => dest.write_all(&[0x10])?,
            Op::Child => dest.write_all(&[0x11])?,
            Op::ParentInverted => dest.write_all(&[0x12])?,
            Op::ChildInverted => dest.write_all(&[0x13])?,
        };
        Ok(())
    }

    fn encoding_length(&self) -> ed::Result<usize> {
        // Header size: 1 (opcode) + 1 (key_len) + 2 (value_len u16) = 4 for small
        // values Header size: 1 (opcode) + 1 (key_len) + 4 (value_len u32) = 6
        // for large values
        Ok(match self {
            Op::Push(Node::Hash(_)) => 1 + HASH_LENGTH,
            Op::Push(Node::KVHash(_)) => 1 + HASH_LENGTH,
            Op::Push(Node::KVDigest(key, _)) => 2 + key.len() + HASH_LENGTH,
            Op::Push(Node::KV(key, value)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len()
            }
            Op::Push(Node::KVValueHash(key, value, _)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + HASH_LENGTH
            }
            Op::Push(Node::KVRefValueHash(key, value, _)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + HASH_LENGTH
            }
            Op::Push(Node::KVValueHashFeatureType(key, value, _, feature_type)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + HASH_LENGTH + feature_type.encoding_length()?
            }
            Op::Push(Node::KVCount(key, value, count)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + count.encoding_length()?
            }
            Op::Push(Node::KVHashCount(_, count)) => 1 + HASH_LENGTH + count.encoding_length()?,
            Op::Push(Node::KVRefValueHashCount(key, value, _, count)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + HASH_LENGTH + count.encoding_length()?
            }
            Op::Push(Node::KVDigestCount(key, _, count)) => {
                2 + key.len() + HASH_LENGTH + count.encoding_length()?
            }
            Op::PushInverted(Node::Hash(_)) => 1 + HASH_LENGTH,
            Op::PushInverted(Node::KVHash(_)) => 1 + HASH_LENGTH,
            Op::PushInverted(Node::KVDigest(key, _)) => 2 + key.len() + HASH_LENGTH,
            Op::PushInverted(Node::KV(key, value)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len()
            }
            Op::PushInverted(Node::KVValueHash(key, value, _)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + HASH_LENGTH
            }
            Op::PushInverted(Node::KVRefValueHash(key, value, _)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + HASH_LENGTH
            }
            Op::PushInverted(Node::KVValueHashFeatureType(key, value, _, feature_type)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + HASH_LENGTH + feature_type.encoding_length()?
            }
            Op::PushInverted(Node::KVCount(key, value, count)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + count.encoding_length()?
            }
            Op::PushInverted(Node::KVHashCount(_, count)) => {
                1 + HASH_LENGTH + count.encoding_length()?
            }
            Op::PushInverted(Node::KVRefValueHashCount(key, value, _, count)) => {
                let header = if value.len() < 65536 { 4 } else { 6 };
                header + key.len() + value.len() + HASH_LENGTH + count.encoding_length()?
            }
            Op::PushInverted(Node::KVDigestCount(key, _, count)) => {
                2 + key.len() + HASH_LENGTH + count.encoding_length()?
            }
            Op::Parent => 1,
            Op::Child => 1,
            Op::ParentInverted => 1,
            Op::ChildInverted => 1,
        })
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Decode for Op {
    fn decode<R: Read>(mut input: R) -> ed::Result<Self> {
        let variant: u8 = Decode::decode(&mut input)?;

        Ok(match variant {
            0x01 => {
                let mut hash = [0; HASH_LENGTH];
                input.read_exact(&mut hash)?;
                Self::Push(Node::Hash(hash))
            }
            0x02 => {
                let mut hash = [0; HASH_LENGTH];
                input.read_exact(&mut hash)?;
                Self::Push(Node::KVHash(hash))
            }
            0x03 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                Self::Push(Node::KV(key, value))
            }
            0x04 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::Push(Node::KVValueHash(key, value, value_hash))
            }
            0x05 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::Push(Node::KVDigest(key, value_hash))
            }
            0x06 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::Push(Node::KVRefValueHash(key, value, value_hash))
            }
            0x07 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let tree_feature_type = TreeFeatureType::decode(input)?;
                Self::Push(Node::KVValueHashFeatureType(
                    key,
                    value,
                    value_hash,
                    tree_feature_type,
                ))
            }
            0x08 => {
                let mut hash = [0; HASH_LENGTH];
                input.read_exact(&mut hash)?;
                Self::PushInverted(Node::Hash(hash))
            }
            0x09 => {
                let mut hash = [0; HASH_LENGTH];
                input.read_exact(&mut hash)?;
                Self::PushInverted(Node::KVHash(hash))
            }
            0x0a => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                Self::PushInverted(Node::KV(key, value))
            }
            0x0b => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::PushInverted(Node::KVValueHash(key, value, value_hash))
            }
            0x0c => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::PushInverted(Node::KVDigest(key, value_hash))
            }
            0x0d => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::PushInverted(Node::KVRefValueHash(key, value, value_hash))
            }
            0x0e => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let tree_feature_type = TreeFeatureType::decode(input)?;
                Self::PushInverted(Node::KVValueHashFeatureType(
                    key,
                    value,
                    value_hash,
                    tree_feature_type,
                ))
            }
            0x14 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let count: u64 = Decode::decode(&mut input)?;

                Self::Push(Node::KVCount(key, value, count))
            }
            0x15 => {
                let mut kv_hash = [0; HASH_LENGTH];
                input.read_exact(&mut kv_hash)?;
                let count: u64 = Decode::decode(&mut input)?;

                Self::Push(Node::KVHashCount(kv_hash, count))
            }
            0x16 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let count: u64 = Decode::decode(&mut input)?;

                Self::PushInverted(Node::KVCount(key, value, count))
            }
            0x17 => {
                let mut kv_hash = [0; HASH_LENGTH];
                input.read_exact(&mut kv_hash)?;
                let count: u64 = Decode::decode(&mut input)?;

                Self::PushInverted(Node::KVHashCount(kv_hash, count))
            }
            0x18 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let count: u64 = Decode::decode(&mut input)?;
                Self::Push(Node::KVRefValueHashCount(key, value, value_hash, count))
            }
            0x19 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u16 = Decode::decode(&mut input)?;
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let count: u64 = Decode::decode(&mut input)?;
                Self::PushInverted(Node::KVRefValueHashCount(key, value, value_hash, count))
            }
            0x1a => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let count: u64 = Decode::decode(&mut input)?;
                Self::Push(Node::KVDigestCount(key, value_hash, count))
            }
            0x1b => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let count: u64 = Decode::decode(&mut input)?;
                Self::PushInverted(Node::KVDigestCount(key, value_hash, count))
            }

            // Large value variants (value_len as u32)
            // Push large variants: 0x20-0x25
            0x20 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x20));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                Self::Push(Node::KV(key, value))
            }
            0x21 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x21));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::Push(Node::KVValueHash(key, value, value_hash))
            }
            0x22 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x22));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::Push(Node::KVRefValueHash(key, value, value_hash))
            }
            0x23 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x23));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let tree_feature_type = TreeFeatureType::decode(input)?;
                Self::Push(Node::KVValueHashFeatureType(
                    key,
                    value,
                    value_hash,
                    tree_feature_type,
                ))
            }
            0x24 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x24));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let count: u64 = Decode::decode(&mut input)?;

                Self::Push(Node::KVCount(key, value, count))
            }
            0x25 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x25));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let count: u64 = Decode::decode(&mut input)?;
                Self::Push(Node::KVRefValueHashCount(key, value, value_hash, count))
            }

            // PushInverted large variants: 0x28-0x2d
            0x28 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x28));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                Self::PushInverted(Node::KV(key, value))
            }
            0x29 => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x29));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::PushInverted(Node::KVValueHash(key, value, value_hash))
            }
            0x2a => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x2a));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                Self::PushInverted(Node::KVRefValueHash(key, value, value_hash))
            }
            0x2b => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x2b));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let tree_feature_type = TreeFeatureType::decode(input)?;
                Self::PushInverted(Node::KVValueHashFeatureType(
                    key,
                    value,
                    value_hash,
                    tree_feature_type,
                ))
            }
            0x2c => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x2c));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let count: u64 = Decode::decode(&mut input)?;

                Self::PushInverted(Node::KVCount(key, value, count))
            }
            0x2d => {
                let key_len: u8 = Decode::decode(&mut input)?;
                let mut key = vec![0; key_len as usize];
                input.read_exact(key.as_mut_slice())?;

                let value_len: u32 = Decode::decode(&mut input)?;
                if value_len > MAX_VALUE_LEN {
                    return Err(ed::Error::UnexpectedByte(0x2d));
                }
                let mut value = vec![0; value_len as usize];
                input.read_exact(value.as_mut_slice())?;

                let mut value_hash = [0; HASH_LENGTH];
                input.read_exact(&mut value_hash)?;

                let count: u64 = Decode::decode(&mut input)?;
                Self::PushInverted(Node::KVRefValueHashCount(key, value, value_hash, count))
            }

            0x10 => Self::Parent,
            0x11 => Self::Child,
            0x12 => Self::ParentInverted,
            0x13 => Self::ChildInverted,
            // TODO: Remove dependency on ed and throw an internal error
            _ => return Err(ed::Error::UnexpectedByte(variant)),
        })
    }
}

#[cfg(feature = "minimal")]
impl Terminated for Op {}

impl Op {
    #[cfg(feature = "minimal")]
    fn encode_into<W: Write>(&self, dest: &mut W) -> Result<(), Error> {
        Encode::encode_into(self, dest).map_err(|e| match e {
            EdError::UnexpectedByte(byte) => Error::ProofCreationError(format!(
                "failed to encode an proofs::Op structure (UnexpectedByte: {byte})"
            )),
            EdError::IOError(error) => Error::ProofCreationError(format!(
                "failed to encode an proofs::Op structure ({error})"
            )),
        })
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    fn encoding_length(&self) -> usize {
        Encode::encoding_length(self).unwrap()
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    /// Decode
    pub fn decode(bytes: &[u8]) -> Result<Self, Error> {
        Decode::decode(bytes).map_err(|e| match e {
            EdError::UnexpectedByte(byte) => Error::ProofCreationError(format!(
                "failed to decode an proofs::Op structure (UnexpectedByte: {byte})"
            )),
            EdError::IOError(error) => Error::ProofCreationError(format!(
                "failed to decode an proofs::Op structure ({error})"
            )),
        })
    }
}

#[cfg(feature = "minimal")]
/// Encode into
pub fn encode_into<'a, T: Iterator<Item = &'a Op>>(ops: T, output: &mut Vec<u8>) {
    for op in ops {
        op.encode_into(output).unwrap();
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
/// Decoder
pub struct Decoder<'a> {
    offset: usize,
    bytes: &'a [u8],
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl<'a> Decoder<'a> {
    /// New decoder
    pub const fn new(proof_bytes: &'a [u8]) -> Self {
        Decoder {
            offset: 0,
            bytes: proof_bytes,
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Iterator for Decoder<'_> {
    type Item = Result<Op, Error>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.offset >= self.bytes.len() {
            return None;
        }

        Some((|| {
            let bytes = &self.bytes[self.offset..];
            let op = Op::decode(bytes)?;
            self.offset += op.encoding_length();
            Ok(op)
        })())
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod test {
    use super::super::{Node, Op};
    use crate::{
        proofs::Decoder,
        tree::HASH_LENGTH,
        TreeFeatureType::{BasicMerkNode, SummedMerkNode},
    };

    #[test]
    fn encode_push_hash() {
        let op = Op::Push(Node::Hash([123; HASH_LENGTH]));
        assert_eq!(op.encoding_length(), 1 + HASH_LENGTH);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x01, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123
            ]
        );
    }

    #[test]
    fn encode_push_kvhash() {
        let op = Op::Push(Node::KVHash([123; HASH_LENGTH]));
        assert_eq!(op.encoding_length(), 1 + HASH_LENGTH);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x02, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123
            ]
        );
    }

    #[test]
    fn encode_push_kvdigest() {
        let op = Op::Push(Node::KVDigest(vec![1, 2, 3], [123; HASH_LENGTH]));
        assert_eq!(op.encoding_length(), 5 + HASH_LENGTH);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x05, 3, 1, 2, 3, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123, 123, 123
            ]
        );
    }

    #[test]
    fn encode_push_kv() {
        let op = Op::Push(Node::KV(vec![1, 2, 3], vec![4, 5, 6]));
        assert_eq!(op.encoding_length(), 10);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes, vec![0x03, 3, 1, 2, 3, 0, 3, 4, 5, 6]);
    }

    #[test]
    fn encode_push_kvvaluehash() {
        let op = Op::Push(Node::KVValueHash(vec![1, 2, 3], vec![4, 5, 6], [0; 32]));
        assert_eq!(op.encoding_length(), 42);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x04, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            ]
        )
    }

    #[test]
    fn encode_push_kvvaluerefhash() {
        let op = Op::Push(Node::KVRefValueHash(vec![1, 2, 3], vec![4, 5, 6], [0; 32]));
        assert_eq!(op.encoding_length(), 42);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x06, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            ]
        )
    }

    #[test]
    fn encode_push_kvvalue_hash_feature_type() {
        let op = Op::Push(Node::KVValueHashFeatureType(
            vec![1, 2, 3],
            vec![4, 5, 6],
            [0; 32],
            BasicMerkNode,
        ));
        assert_eq!(op.encoding_length(), 43);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x07, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            ]
        );

        let op = Op::Push(Node::KVValueHashFeatureType(
            vec![1, 2, 3],
            vec![4, 5, 6],
            [0; 32],
            SummedMerkNode(6),
        ));
        assert_eq!(op.encoding_length(), 44);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x07, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 12
            ]
        )
    }

    #[test]
    fn encode_push_inverted_hash() {
        let op = Op::PushInverted(Node::Hash([123; HASH_LENGTH]));
        assert_eq!(op.encoding_length(), 1 + HASH_LENGTH);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x08, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123
            ]
        );
    }

    #[test]
    fn encode_push_inverted_kvhash() {
        let op = Op::PushInverted(Node::KVHash([123; HASH_LENGTH]));
        assert_eq!(op.encoding_length(), 1 + HASH_LENGTH);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x09, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123
            ]
        );
    }

    #[test]
    fn encode_push_inverted_kvdigest() {
        let op = Op::PushInverted(Node::KVDigest(vec![1, 2, 3], [123; HASH_LENGTH]));
        assert_eq!(op.encoding_length(), 5 + HASH_LENGTH);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x0c, 3, 1, 2, 3, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
                123, 123, 123
            ]
        );
    }

    #[test]
    fn encode_push_inverted_kv() {
        let op = Op::PushInverted(Node::KV(vec![1, 2, 3], vec![4, 5, 6]));
        assert_eq!(op.encoding_length(), 10);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes, vec![0x0a, 3, 1, 2, 3, 0, 3, 4, 5, 6]);
    }

    #[test]
    fn encode_push_inverted_kvvaluehash() {
        let op = Op::PushInverted(Node::KVValueHash(vec![1, 2, 3], vec![4, 5, 6], [0; 32]));
        assert_eq!(op.encoding_length(), 42);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x0b, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            ]
        )
    }

    #[test]
    fn encode_push_inverted_kvvalue_hash_feature_type() {
        let op = Op::PushInverted(Node::KVValueHashFeatureType(
            vec![1, 2, 3],
            vec![4, 5, 6],
            [0; 32],
            BasicMerkNode,
        ));
        assert_eq!(op.encoding_length(), 43);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x0e, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            ]
        );

        let op = Op::PushInverted(Node::KVValueHashFeatureType(
            vec![1, 2, 3],
            vec![4, 5, 6],
            [0; 32],
            SummedMerkNode(5),
        ));
        assert_eq!(op.encoding_length(), 44);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x0e, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 10
            ]
        );
    }

    #[test]
    fn encode_push_inverted_kvvaluerefhash() {
        let op = Op::PushInverted(Node::KVRefValueHash(vec![1, 2, 3], vec![4, 5, 6], [0; 32]));
        assert_eq!(op.encoding_length(), 42);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(
            bytes,
            vec![
                0x0d, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            ]
        )
    }

    #[test]
    fn encode_parent() {
        let op = Op::Parent;
        assert_eq!(op.encoding_length(), 1);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes, vec![0x10]);
    }

    #[test]
    fn encode_child() {
        let op = Op::Child;
        assert_eq!(op.encoding_length(), 1);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes, vec![0x11]);
    }

    #[test]
    fn encode_parent_inverted() {
        let op = Op::ParentInverted;
        assert_eq!(op.encoding_length(), 1);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes, vec![0x12]);
    }

    #[test]
    fn encode_child_inverted() {
        let op = Op::ChildInverted;
        assert_eq!(op.encoding_length(), 1);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes, vec![0x13]);
    }

    #[test]
    #[should_panic]
    fn encode_push_kv_long_key() {
        let op = Op::Push(Node::KV(vec![123; 300], vec![4, 5, 6]));
        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
    }

    #[test]
    fn decode_push_hash() {
        let bytes = [
            0x01, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
            123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::Push(Node::Hash([123; HASH_LENGTH])));
    }

    #[test]
    fn decode_push_kvhash() {
        let bytes = [
            0x02, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
            123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::Push(Node::KVHash([123; HASH_LENGTH])));
    }

    #[test]
    fn decode_push_kvdigest() {
        let bytes = [
            0x05, 3, 1, 2, 3, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
            123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
            123,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::Push(Node::KVDigest(vec![1, 2, 3], [123; HASH_LENGTH]))
        );
    }

    #[test]
    fn decode_push_kv() {
        let bytes = [0x03, 3, 1, 2, 3, 0, 3, 4, 5, 6];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::Push(Node::KV(vec![1, 2, 3], vec![4, 5, 6])));
    }

    #[test]
    fn decode_push_kvvaluehash() {
        let bytes = [
            0x04, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::Push(Node::KVValueHash(vec![1, 2, 3], vec![4, 5, 6], [0; 32]))
        );
    }

    #[test]
    fn decode_push_kvvaluerefhash() {
        let bytes = [
            0x06, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::Push(Node::KVRefValueHash(vec![1, 2, 3], vec![4, 5, 6], [0; 32]))
        );
    }

    #[test]
    fn decode_push_kvvalue_hash_feature_type() {
        let bytes = [
            0x07, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::Push(Node::KVValueHashFeatureType(
                vec![1, 2, 3],
                vec![4, 5, 6],
                [0; 32],
                BasicMerkNode
            ))
        );

        let bytes = [
            0x07, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 12,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::Push(Node::KVValueHashFeatureType(
                vec![1, 2, 3],
                vec![4, 5, 6],
                [0; 32],
                SummedMerkNode(6)
            ))
        );
    }

    #[test]
    fn decode_push_inverted_hash() {
        let bytes = [
            0x08, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
            123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::PushInverted(Node::Hash([123; HASH_LENGTH])));
    }

    #[test]
    fn decode_push_inverted_kvhash() {
        let bytes = [
            0x09, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
            123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::PushInverted(Node::KVHash([123; HASH_LENGTH])));
    }

    #[test]
    fn decode_push_inverted_kvdigest() {
        let bytes = [
            0x0c, 3, 1, 2, 3, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
            123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
            123,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::PushInverted(Node::KVDigest(vec![1, 2, 3], [123; HASH_LENGTH]))
        );
    }

    #[test]
    fn decode_push_inverted_kv() {
        let bytes = [0x0a, 3, 1, 2, 3, 0, 3, 4, 5, 6];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::PushInverted(Node::KV(vec![1, 2, 3], vec![4, 5, 6])));
    }

    #[test]
    fn decode_push_inverted_kvvaluehash() {
        let bytes = [
            0x0b, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::PushInverted(Node::KVValueHash(vec![1, 2, 3], vec![4, 5, 6], [0; 32]))
        );
    }

    #[test]
    fn decode_push_inverted_kvvaluerefhash() {
        let bytes = [
            0x0d, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::PushInverted(Node::KVRefValueHash(vec![1, 2, 3], vec![4, 5, 6], [0; 32]))
        );
    }

    #[test]
    fn decode_push_inverted_kvvalue_hash_feature_type() {
        let bytes = [
            0x0e, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::PushInverted(Node::KVValueHashFeatureType(
                vec![1, 2, 3],
                vec![4, 5, 6],
                [0; 32],
                BasicMerkNode
            ))
        );

        let bytes = [
            0x0e, 3, 1, 2, 3, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 12,
        ];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(
            op,
            Op::PushInverted(Node::KVValueHashFeatureType(
                vec![1, 2, 3],
                vec![4, 5, 6],
                [0; 32],
                SummedMerkNode(6)
            ))
        );
    }

    #[test]
    fn decode_parent() {
        let bytes = [0x10];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::Parent);
    }

    #[test]
    fn decode_child() {
        let bytes = [0x11];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::Child);
    }

    #[test]
    fn decode_multiple_child() {
        let bytes = [0x11, 0x11, 0x11, 0x10];
        let decoder = Decoder {
            bytes: &bytes,
            offset: 0,
        };

        let mut vecop = vec![];
        for op in decoder {
            match op {
                Ok(op) => vecop.push(op),
                Err(e) => eprintln!("Error decoding: {:?}", e),
            }
        }
        assert_eq!(vecop, vec![Op::Child, Op::Child, Op::Child, Op::Parent]);
    }

    #[test]
    fn decode_parent_inverted() {
        let bytes = [0x12];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::ParentInverted);
    }

    #[test]
    fn decode_child_inverted() {
        let bytes = [0x13];
        let op = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(op, Op::ChildInverted);
    }

    #[test]
    fn decode_unknown() {
        let bytes = [0x88];
        assert!(Op::decode(&bytes[..]).is_err());
    }

    #[test]
    fn encode_decode_push_kvcount() {
        let op = Op::Push(Node::KVCount(vec![1, 2, 3], vec![4, 5, 6], 42));
        // 1 opcode + 1 key_len + key + 2 value_len + value + 8 count
        let expected_length = 4 + 3 + 3 + 8;
        assert_eq!(op.encoding_length(), expected_length);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes.len(), expected_length);
        assert_eq!(bytes[0], 0x14); // Check opcode

        let decoded = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(decoded, op);
    }

    #[test]
    fn encode_decode_push_kvhashcount() {
        let op = Op::Push(Node::KVHashCount([123; HASH_LENGTH], 42));
        let expected_length = 1 + HASH_LENGTH + 8; // 1 opcode + 32 hash + 8 count
        assert_eq!(op.encoding_length(), expected_length);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes.len(), expected_length);
        assert_eq!(bytes[0], 0x15); // Check opcode

        let decoded = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(decoded, op);
    }

    #[test]
    fn encode_decode_push_inverted_kvcount() {
        let op = Op::PushInverted(Node::KVCount(vec![1, 2, 3], vec![4, 5, 6], 42));
        // 1 opcode + 1 key_len + key + 2 value_len + value + 8 count
        let expected_length = 4 + 3 + 3 + 8;
        assert_eq!(op.encoding_length(), expected_length);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes.len(), expected_length);
        assert_eq!(bytes[0], 0x16); // Check opcode

        let decoded = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(decoded, op);
    }

    #[test]
    fn encode_decode_push_inverted_kvhashcount() {
        let op = Op::PushInverted(Node::KVHashCount([123; HASH_LENGTH], 42));
        let expected_length = 1 + HASH_LENGTH + 8; // 1 opcode + 32 hash + 8 count
        assert_eq!(op.encoding_length(), expected_length);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes.len(), expected_length);
        assert_eq!(bytes[0], 0x17); // Check opcode

        let decoded = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(decoded, op);
    }

    #[test]
    fn decoder_with_count_nodes() {
        let ops = vec![
            Op::Push(Node::KVCount(vec![1, 2, 3], vec![4, 5, 6], 42)),
            Op::Push(Node::KVHashCount([123; HASH_LENGTH], 100)),
            Op::Child,
            Op::PushInverted(Node::KVCount(vec![7, 8, 9], vec![10, 11, 12], 200)),
            Op::Parent,
        ];

        let mut encoded = vec![];
        for op in &ops {
            op.encode_into(&mut encoded).unwrap();
        }

        let decoder = Decoder::new(&encoded);
        let decoded_ops: Result<Vec<Op>, _> = decoder.collect();
        assert!(decoded_ops.is_ok());
        assert_eq!(decoded_ops.unwrap(), ops);
    }

    #[test]
    fn encode_decode_push_kvrefvaluehash_count() {
        let op = Op::Push(Node::KVRefValueHashCount(
            vec![1, 2, 3],
            vec![4, 5, 6],
            [0; 32],
            42,
        ));
        // 1 opcode + 1 key_len + key + 2 value_len + value + 32 hash + 8 count
        let expected_length = 4 + 3 + 3 + HASH_LENGTH + 8;
        assert_eq!(op.encoding_length(), expected_length);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes.len(), expected_length);
        assert_eq!(bytes[0], 0x18); // Check opcode

        let decoded = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(decoded, op);
    }

    #[test]
    fn encode_decode_push_inverted_kvrefvaluehash_count() {
        let op = Op::PushInverted(Node::KVRefValueHashCount(
            vec![1, 2, 3],
            vec![4, 5, 6],
            [0; 32],
            100,
        ));
        // 1 opcode + 1 key_len + key + 2 value_len + value + 32 hash + 8 count
        let expected_length = 4 + 3 + 3 + HASH_LENGTH + 8;
        assert_eq!(op.encoding_length(), expected_length);

        let mut bytes = vec![];
        op.encode_into(&mut bytes).unwrap();
        assert_eq!(bytes.len(), expected_length);
        assert_eq!(bytes[0], 0x19); // Check opcode

        let decoded = Op::decode(&bytes[..]).expect("decode failed");
        assert_eq!(decoded, op);
    }
}
