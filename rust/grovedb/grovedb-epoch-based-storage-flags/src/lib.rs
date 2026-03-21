//! Flags

pub mod error;
mod split_removal_bytes;
mod update_element_flags;

use crate::{
    error::StorageFlagsError,
    StorageFlags::{MultiEpoch, MultiEpochOwned, SingleEpoch, SingleEpochOwned},
};

const DEFAULT_HASH_SIZE_U32: u32 = 32;

/// Optional meta-data to be stored per element
pub type ElementFlags = Vec<u8>;

use std::{borrow::Cow, cmp::Ordering, collections::BTreeMap, fmt};

use grovedb_costs::storage_cost::removal::{
    StorageRemovalPerEpochByIdentifier, StorageRemovedBytes,
    StorageRemovedBytes::{NoStorageRemoval, SectionedStorageRemoval},
};
use integer_encoding::VarInt;
use intmap::IntMap;

type EpochIndex = u16;

type BaseEpoch = EpochIndex;

type BytesAddedInEpoch = u32;

type OwnerId = [u8; 32];

/// The size of single epoch flags
pub const SINGLE_EPOCH_FLAGS_SIZE: u32 = 3;

/// The minimum size of the non-base flags
pub const MINIMUM_NON_BASE_FLAGS_SIZE: u32 = 3;

/// Storage flags
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StorageFlags {
    /// Single epoch
    /// represented as byte 0
    SingleEpoch(BaseEpoch),

    /// Multi epoch
    /// represented as byte 1
    MultiEpoch(BaseEpoch, BTreeMap<EpochIndex, BytesAddedInEpoch>),

    /// Single epoch owned
    /// represented as byte 2
    SingleEpochOwned(BaseEpoch, OwnerId),

    /// Multi epoch owned
    /// represented as byte 3
    MultiEpochOwned(BaseEpoch, BTreeMap<EpochIndex, BytesAddedInEpoch>, OwnerId),
}

impl fmt::Display for StorageFlags {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StorageFlags::SingleEpoch(base_epoch) => {
                write!(f, "SingleEpoch(BaseEpoch: {})", base_epoch)
            }
            StorageFlags::MultiEpoch(base_epoch, epochs) => {
                write!(f, "MultiEpoch(BaseEpoch: {}, Epochs: ", base_epoch)?;
                for (index, bytes) in epochs {
                    write!(f, "[EpochIndex: {}, BytesAdded: {}] ", index, bytes)?;
                }
                write!(f, ")")
            }
            StorageFlags::SingleEpochOwned(base_epoch, owner_id) => {
                write!(
                    f,
                    "SingleEpochOwned(BaseEpoch: {}, OwnerId: {})",
                    base_epoch,
                    hex::encode(owner_id)
                )
            }
            StorageFlags::MultiEpochOwned(base_epoch, epochs, owner_id) => {
                write!(f, "MultiEpochOwned(BaseEpoch: {}, Epochs: ", base_epoch)?;
                for (index, bytes) in epochs {
                    write!(f, "[EpochIndex: {}, BytesAdded: {}] ", index, bytes)?;
                }
                write!(f, ", OwnerId: {})", hex::encode(owner_id))
            }
        }
    }
}

/// MergingOwnersStrategy decides which owner to keep during a merge
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum MergingOwnersStrategy {
    #[default]
    /// Raise an issue that owners of nodes are different
    RaiseIssue,
    /// Use the original owner id
    UseOurs,
    /// Use the new owner id
    UseTheirs,
}

impl StorageFlags {
    /// Create new single epoch storage flags
    pub fn new_single_epoch(epoch: BaseEpoch, maybe_owner_id: Option<OwnerId>) -> Self {
        match maybe_owner_id {
            None => SingleEpoch(epoch),
            Some(owner_id) => SingleEpochOwned(epoch, owner_id),
        }
    }

    /// Sets the owner id if we have owned storage flags
    pub fn set_owner_id(&mut self, owner_id: OwnerId) {
        match self {
            SingleEpochOwned(_, previous_owner_id) | MultiEpochOwned(_, _, previous_owner_id) => {
                *previous_owner_id = owner_id;
            }
            _ => {}
        }
    }

    fn combine_owner_id<'a>(
        &'a self,
        rhs: &'a Self,
        merging_owners_strategy: MergingOwnersStrategy,
    ) -> Result<Option<&'a OwnerId>, StorageFlagsError> {
        if let Some(our_owner_id) = self.owner_id() {
            if let Some(other_owner_id) = rhs.owner_id() {
                if our_owner_id != other_owner_id {
                    match merging_owners_strategy {
                        MergingOwnersStrategy::RaiseIssue => {
                            Err(StorageFlagsError::MergingStorageFlagsFromDifferentOwners(
                                "can not merge from different owners".to_string(),
                            ))
                        }
                        MergingOwnersStrategy::UseOurs => Ok(Some(our_owner_id)),
                        MergingOwnersStrategy::UseTheirs => Ok(Some(other_owner_id)),
                    }
                } else {
                    Ok(Some(our_owner_id))
                }
            } else {
                Ok(Some(our_owner_id))
            }
        } else if let Some(other_owner_id) = rhs.owner_id() {
            Ok(Some(other_owner_id))
        } else {
            Ok(None)
        }
    }

    fn combine_non_base_epoch_bytes(
        &self,
        rhs: &Self,
    ) -> Option<BTreeMap<EpochIndex, BytesAddedInEpoch>> {
        if let Some(our_epoch_index_map) = self.epoch_index_map() {
            if let Some(other_epoch_index_map) = rhs.epoch_index_map() {
                let mut combined_index_map = our_epoch_index_map.clone();
                other_epoch_index_map
                    .iter()
                    .for_each(|(epoch_index, bytes_added)| {
                        // Simply insert the value from rhs, overwriting any existing value
                        combined_index_map.insert(*epoch_index, *bytes_added);
                    });
                // println!(
                //     "         >combine_non_base_epoch_bytes: self:{:?} & rhs:{:?} -> {:?}",
                //     our_epoch_index_map, other_epoch_index_map, combined_index_map
                // );
                Some(combined_index_map)
            } else {
                Some(our_epoch_index_map.clone())
            }
        } else {
            rhs.epoch_index_map().cloned()
        }
    }

    fn combine_same_base_epoch(
        &self,
        rhs: Self,
        merging_owners_strategy: MergingOwnersStrategy,
    ) -> Result<Self, StorageFlagsError> {
        let base_epoch = *self.base_epoch();
        let owner_id = self.combine_owner_id(&rhs, merging_owners_strategy)?;
        let other_epoch_bytes = self.combine_non_base_epoch_bytes(&rhs);

        match (owner_id, other_epoch_bytes) {
            (None, None) => Ok(SingleEpoch(base_epoch)),
            (Some(owner_id), None) => Ok(SingleEpochOwned(base_epoch, *owner_id)),
            (None, Some(other_epoch_bytes)) => Ok(MultiEpoch(base_epoch, other_epoch_bytes)),
            (Some(owner_id), Some(other_epoch_bytes)) => {
                Ok(MultiEpochOwned(base_epoch, other_epoch_bytes, *owner_id))
            }
        }
    }

    fn combine_with_higher_base_epoch(
        &self,
        rhs: Self,
        added_bytes: u32,
        merging_owners_strategy: MergingOwnersStrategy,
    ) -> Result<Self, StorageFlagsError> {
        let base_epoch = *self.base_epoch();
        let epoch_with_adding_bytes = rhs.base_epoch();
        let owner_id = self.combine_owner_id(&rhs, merging_owners_strategy)?;
        let mut other_epoch_bytes = self.combine_non_base_epoch_bytes(&rhs).unwrap_or_default();
        let original_value = other_epoch_bytes.remove(epoch_with_adding_bytes);
        match original_value {
            None => other_epoch_bytes.insert(*epoch_with_adding_bytes, added_bytes),
            Some(original_bytes) => {
                other_epoch_bytes.insert(*epoch_with_adding_bytes, original_bytes + added_bytes)
            }
        };
        // println!(
        //     "         >combine_with_higher_base_epoch added_bytes:{} self:{:?} &
        // rhs:{:?} -> {:?}",     added_bytes,
        //     self.epoch_index_map(),
        //     rhs.epoch_index_map(),
        //     other_epoch_bytes
        // );

        match owner_id {
            None => Ok(MultiEpoch(base_epoch, other_epoch_bytes)),
            Some(owner_id) => Ok(MultiEpochOwned(base_epoch, other_epoch_bytes, *owner_id)),
        }
    }

    fn combine_with_higher_base_epoch_remove_bytes(
        self,
        rhs: Self,
        removed_bytes: &StorageRemovedBytes,
        merging_owners_strategy: MergingOwnersStrategy,
    ) -> Result<Self, StorageFlagsError> {
        if matches!(&self, &SingleEpoch(_) | &SingleEpochOwned(..)) {
            return Ok(self);
        }
        let base_epoch = *self.base_epoch();
        let owner_id = self.combine_owner_id(&rhs, merging_owners_strategy)?;
        let mut other_epoch_bytes = self.combine_non_base_epoch_bytes(&rhs).unwrap_or_default();
        if let SectionedStorageRemoval(sectioned_bytes_by_identifier) = removed_bytes {
            if sectioned_bytes_by_identifier.len() > 1 {
                return Err(StorageFlagsError::MergingStorageFlagsFromDifferentOwners(
                    "can not remove bytes when there is no epoch".to_string(),
                ));
            }
            // we must use our owner id, because we would be removing bytes from it
            let identifier = self.owner_id().copied().unwrap_or_default();
            let sectioned_bytes = sectioned_bytes_by_identifier.get(&identifier).ok_or(
                StorageFlagsError::MergingStorageFlagsFromDifferentOwners(
                    "can not remove bytes when there is no epoch".to_string(),
                ),
            )?;
            let mut keys_to_remove = Vec::new(); // To store the keys that need to be removed

            sectioned_bytes
                .iter()
                .try_for_each(|(epoch, removed_bytes)| {
                    if epoch == base_epoch {
                        return Ok::<(), StorageFlagsError>(());
                    }
                    let bytes_added_in_epoch = other_epoch_bytes.get_mut(&epoch).ok_or(
                        StorageFlagsError::RemovingAtEpochWithNoAssociatedStorage(format!(
                            "can not remove bytes when there is no epoch number [{}]",
                            epoch
                        )),
                    )?;

                    let desired_bytes_in_epoch = bytes_added_in_epoch
                        .checked_sub(*removed_bytes)
                        .ok_or(StorageFlagsError::StorageFlagsOverflow(
                        "can't remove more bytes than exist at that epoch".to_string(),
                    ))?;

                    if desired_bytes_in_epoch <= MINIMUM_NON_BASE_FLAGS_SIZE {
                        // Collect the key to remove later
                        keys_to_remove.push(epoch);
                    } else {
                        *bytes_added_in_epoch = desired_bytes_in_epoch;
                    }

                    Ok::<(), StorageFlagsError>(())
                })?;

            // Now remove the keys after the iteration
            for key in keys_to_remove {
                other_epoch_bytes.remove(&key);
            }
        }
        // println!(
        //     "         >combine_with_higher_base_epoch_remove_bytes: self:{:?} &
        // rhs:{:?} -> {:?}",     self.epoch_index_map(),
        //     rhs.epoch_index_map(),
        //     other_epoch_bytes
        // );

        if other_epoch_bytes.is_empty() {
            match owner_id {
                None => Ok(SingleEpoch(base_epoch)),
                Some(owner_id) => Ok(SingleEpochOwned(base_epoch, *owner_id)),
            }
        } else {
            match owner_id {
                None => Ok(MultiEpoch(base_epoch, other_epoch_bytes)),
                Some(owner_id) => Ok(MultiEpochOwned(base_epoch, other_epoch_bytes, *owner_id)),
            }
        }
    }

    /// Optional combine added bytes
    pub fn optional_combine_added_bytes(
        ours: Option<Self>,
        theirs: Self,
        added_bytes: u32,
        merging_owners_strategy: MergingOwnersStrategy,
    ) -> Result<Self, StorageFlagsError> {
        match ours {
            None => Ok(theirs),
            Some(ours) => {
                Ok(ours.combine_added_bytes(theirs, added_bytes, merging_owners_strategy)?)
            }
        }
    }

    /// Optional combine removed bytes
    pub fn optional_combine_removed_bytes(
        ours: Option<Self>,
        theirs: Self,
        removed_bytes: &StorageRemovedBytes,
        merging_owners_strategy: MergingOwnersStrategy,
    ) -> Result<Self, StorageFlagsError> {
        match ours {
            None => Ok(theirs),
            Some(ours) => {
                Ok(ours.combine_removed_bytes(theirs, removed_bytes, merging_owners_strategy)?)
            }
        }
    }

    /// Combine added bytes
    pub fn combine_added_bytes(
        self,
        rhs: Self,
        added_bytes: u32,
        merging_owners_strategy: MergingOwnersStrategy,
    ) -> Result<Self, StorageFlagsError> {
        match self.base_epoch().cmp(rhs.base_epoch()) {
            Ordering::Equal => self.combine_same_base_epoch(rhs, merging_owners_strategy),
            Ordering::Less => {
                self.combine_with_higher_base_epoch(rhs, added_bytes, merging_owners_strategy)
            }
            Ordering::Greater => Err(
                StorageFlagsError::MergingStorageFlagsWithDifferentBaseEpoch(
                    "can not merge with new item in older base epoch".to_string(),
                ),
            ),
        }
    }

    /// Combine removed bytes
    pub fn combine_removed_bytes(
        self,
        rhs: Self,
        removed_bytes: &StorageRemovedBytes,
        merging_owners_strategy: MergingOwnersStrategy,
    ) -> Result<Self, StorageFlagsError> {
        match self.base_epoch().cmp(rhs.base_epoch()) {
            Ordering::Equal => self.combine_same_base_epoch(rhs, merging_owners_strategy),
            Ordering::Less => self.combine_with_higher_base_epoch_remove_bytes(
                rhs,
                removed_bytes,
                merging_owners_strategy,
            ),
            Ordering::Greater => Err(
                StorageFlagsError::MergingStorageFlagsWithDifferentBaseEpoch(
                    "can not merge with new item in older base epoch".to_string(),
                ),
            ),
        }
    }

    /// Returns base epoch
    pub fn base_epoch(&self) -> &BaseEpoch {
        match self {
            SingleEpoch(base_epoch)
            | MultiEpoch(base_epoch, _)
            | SingleEpochOwned(base_epoch, _)
            | MultiEpochOwned(base_epoch, ..) => base_epoch,
        }
    }

    /// Returns owner id
    pub fn owner_id(&self) -> Option<&OwnerId> {
        match self {
            SingleEpochOwned(_, owner_id) | MultiEpochOwned(_, _, owner_id) => Some(owner_id),
            _ => None,
        }
    }

    /// Returns epoch index map
    pub fn epoch_index_map(&self) -> Option<&BTreeMap<EpochIndex, BytesAddedInEpoch>> {
        match self {
            MultiEpoch(_, epoch_int_map) | MultiEpochOwned(_, epoch_int_map, _) => {
                Some(epoch_int_map)
            }
            _ => None,
        }
    }

    /// Returns optional default storage flags
    pub fn optional_default() -> Option<Self> {
        None
    }

    /// Returns default optional storage flag as ref
    pub fn optional_default_as_ref() -> Option<&'static Self> {
        None
    }

    /// Returns default optional storage flag as ref
    pub fn optional_default_as_cow() -> Option<Cow<'static, Self>> {
        None
    }

    /// Returns type byte
    pub fn type_byte(&self) -> u8 {
        match self {
            SingleEpoch(_) => 0,
            MultiEpoch(..) => 1,
            SingleEpochOwned(..) => 2,
            MultiEpochOwned(..) => 3,
        }
    }

    fn append_to_vec_base_epoch(&self, buffer: &mut Vec<u8>) {
        match self {
            SingleEpoch(base_epoch)
            | MultiEpoch(base_epoch, ..)
            | SingleEpochOwned(base_epoch, ..)
            | MultiEpochOwned(base_epoch, ..) => buffer.extend(base_epoch.to_be_bytes()),
        }
    }

    fn maybe_append_to_vec_epoch_map(&self, buffer: &mut Vec<u8>) {
        match self {
            MultiEpoch(_, epoch_map) | MultiEpochOwned(_, epoch_map, _) => {
                if epoch_map.is_empty() {
                    panic!("this should not be empty");
                }
                epoch_map.iter().for_each(|(epoch_index, bytes_added)| {
                    buffer.extend(epoch_index.to_be_bytes());
                    buffer.extend(bytes_added.encode_var_vec());
                })
            }
            _ => {}
        }
    }

    fn maybe_epoch_map_size(&self) -> u32 {
        let mut size = 0;
        match self {
            MultiEpoch(_, epoch_map) | MultiEpochOwned(_, epoch_map, _) => {
                epoch_map.iter().for_each(|(_epoch_index, bytes_added)| {
                    size += 2;
                    size += bytes_added.encode_var_vec().len() as u32;
                })
            }
            _ => {}
        }
        size
    }

    fn maybe_append_to_vec_owner_id(&self, buffer: &mut Vec<u8>) {
        match self {
            SingleEpochOwned(_, owner_id) | MultiEpochOwned(_, _, owner_id) => {
                buffer.extend(owner_id);
            }
            _ => {}
        }
    }

    fn maybe_owner_id_size(&self) -> u32 {
        match self {
            SingleEpochOwned(..) | MultiEpochOwned(..) => DEFAULT_HASH_SIZE_U32,
            _ => 0,
        }
    }

    /// ApproximateSize
    pub fn approximate_size(
        has_owner_id: bool,
        approximate_changes_and_bytes_count: Option<(u16, u8)>,
    ) -> u32 {
        let mut size = 3; // 1 for type byte, 2 for epoch number
        if has_owner_id {
            size += DEFAULT_HASH_SIZE_U32;
        }
        if let Some((approximate_change_count, bytes_changed_required_size)) =
            approximate_changes_and_bytes_count
        {
            size += (approximate_change_count as u32) * (2 + bytes_changed_required_size as u32)
        }
        size
    }

    /// Serialize storage flags
    pub fn serialize(&self) -> Vec<u8> {
        let mut buffer = vec![self.type_byte()];
        self.maybe_append_to_vec_owner_id(&mut buffer);
        self.append_to_vec_base_epoch(&mut buffer);
        self.maybe_append_to_vec_epoch_map(&mut buffer);
        buffer
    }

    /// Serialize storage flags
    pub fn serialized_size(&self) -> u32 {
        let mut buffer_len = 3; // for type byte and base epoch
        buffer_len += self.maybe_owner_id_size();
        buffer_len += self.maybe_epoch_map_size();
        buffer_len
    }

    /// Deserialize single epoch storage flags from bytes
    pub fn deserialize_single_epoch(data: &[u8]) -> Result<Self, StorageFlagsError> {
        if data.len() != 3 {
            Err(StorageFlagsError::StorageFlagsWrongSize(
                "single epoch must be 3 bytes total".to_string(),
            ))
        } else {
            let epoch = u16::from_be_bytes(data[1..3].try_into().map_err(|_| {
                StorageFlagsError::StorageFlagsWrongSize(
                    "single epoch must be 3 bytes total".to_string(),
                )
            })?);
            Ok(SingleEpoch(epoch))
        }
    }

    /// Deserialize multi epoch storage flags from bytes
    pub fn deserialize_multi_epoch(data: &[u8]) -> Result<Self, StorageFlagsError> {
        let len = data.len();
        if len < 6 {
            Err(StorageFlagsError::StorageFlagsWrongSize(
                "multi epoch must be at least 6 bytes total".to_string(),
            ))
        } else {
            let base_epoch = u16::from_be_bytes(data[1..3].try_into().map_err(|_| {
                StorageFlagsError::StorageFlagsWrongSize(
                    "multi epoch must have enough bytes for the base epoch".to_string(),
                )
            })?);
            let mut offset = 3;
            let mut bytes_per_epoch: BTreeMap<u16, u32> = BTreeMap::default();
            while offset + 2 < len {
                // 2 for epoch size
                let epoch_index =
                    u16::from_be_bytes(data[offset..offset + 2].try_into().map_err(|_| {
                        StorageFlagsError::StorageFlagsWrongSize(
                            "multi epoch must have enough bytes epoch indexes".to_string(),
                        )
                    })?);
                offset += 2;
                let (bytes_at_epoch, bytes_used) = u32::decode_var(&data[offset..]).ok_or(
                    StorageFlagsError::StorageFlagsWrongSize(
                        "multi epoch must have enough bytes for the amount of bytes used"
                            .to_string(),
                    ),
                )?;
                offset += bytes_used;
                bytes_per_epoch.insert(epoch_index, bytes_at_epoch);
            }
            Ok(MultiEpoch(base_epoch, bytes_per_epoch))
        }
    }

    /// Deserialize single epoch owned storage flags from bytes
    pub fn deserialize_single_epoch_owned(data: &[u8]) -> Result<Self, StorageFlagsError> {
        if data.len() != 35 {
            Err(StorageFlagsError::StorageFlagsWrongSize(
                "single epoch owned must be 35 bytes total".to_string(),
            ))
        } else {
            let owner_id: OwnerId = data[1..33].try_into().map_err(|_| {
                StorageFlagsError::StorageFlagsWrongSize(
                    "single epoch owned must be 35 bytes total for owner id".to_string(),
                )
            })?;
            let epoch = u16::from_be_bytes(data[33..35].try_into().map_err(|_| {
                StorageFlagsError::StorageFlagsWrongSize(
                    "single epoch owned must be 35 bytes total for epoch".to_string(),
                )
            })?);
            Ok(SingleEpochOwned(epoch, owner_id))
        }
    }

    /// Deserialize multi epoch owned storage flags from bytes
    pub fn deserialize_multi_epoch_owned(data: &[u8]) -> Result<Self, StorageFlagsError> {
        let len = data.len();
        if len < 38 {
            Err(StorageFlagsError::StorageFlagsWrongSize(
                "multi epoch owned must be at least 38 bytes total".to_string(),
            ))
        } else {
            let owner_id: OwnerId = data[1..33].try_into().map_err(|_| {
                StorageFlagsError::StorageFlagsWrongSize(
                    "multi epoch owned must be 38 bytes total for owner id".to_string(),
                )
            })?;
            let base_epoch = u16::from_be_bytes(data[33..35].try_into().map_err(|_| {
                StorageFlagsError::StorageFlagsWrongSize(
                    "multi epoch must have enough bytes for the base epoch".to_string(),
                )
            })?);
            let mut offset = 35;
            let mut bytes_per_epoch: BTreeMap<u16, u32> = BTreeMap::default();
            while offset + 2 < len {
                // 2 for epoch size
                let epoch_index =
                    u16::from_be_bytes(data[offset..offset + 2].try_into().map_err(|_| {
                        StorageFlagsError::StorageFlagsWrongSize(
                            "multi epoch must have enough bytes epoch indexes".to_string(),
                        )
                    })?);
                offset += 2;
                let (bytes_at_epoch, bytes_used) = u32::decode_var(&data[offset..]).ok_or(
                    StorageFlagsError::StorageFlagsWrongSize(
                        "multi epoch must have enough bytes for the amount of bytes used"
                            .to_string(),
                    ),
                )?;
                offset += bytes_used;
                bytes_per_epoch.insert(epoch_index, bytes_at_epoch);
            }
            Ok(MultiEpochOwned(base_epoch, bytes_per_epoch, owner_id))
        }
    }

    /// Deserialize storage flags from bytes
    pub fn deserialize(data: &[u8]) -> Result<Option<Self>, StorageFlagsError> {
        let first_byte = data.first();
        match first_byte {
            None => Ok(None),
            Some(first_byte) => match *first_byte {
                0 => Ok(Some(Self::deserialize_single_epoch(data)?)),
                1 => Ok(Some(Self::deserialize_multi_epoch(data)?)),
                2 => Ok(Some(Self::deserialize_single_epoch_owned(data)?)),
                3 => Ok(Some(Self::deserialize_multi_epoch_owned(data)?)),
                _ => Err(StorageFlagsError::DeserializeUnknownStorageFlagsType(
                    "unknown storage flags serialization".to_string(),
                )),
            },
        }
    }

    /// Creates storage flags from a slice.
    pub fn from_slice(data: &[u8]) -> Result<Option<Self>, StorageFlagsError> {
        Self::deserialize(data)
    }

    /// Creates storage flags from element flags.
    pub fn from_element_flags_ref(data: &ElementFlags) -> Result<Option<Self>, StorageFlagsError> {
        Self::from_slice(data.as_slice())
    }

    /// Create Storage flags from optional element flags ref
    pub fn map_some_element_flags_ref(
        data: &Option<ElementFlags>,
    ) -> Result<Option<Self>, StorageFlagsError> {
        match data {
            None => Ok(None),
            Some(data) => Self::from_slice(data.as_slice()),
        }
    }

    /// Create Storage flags from optional element flags ref
    pub fn map_cow_some_element_flags_ref(
        data: &Option<ElementFlags>,
    ) -> Result<Option<Cow<'_, Self>>, StorageFlagsError> {
        match data {
            None => Ok(None),
            Some(data) => Self::from_slice(data.as_slice()).map(|option| option.map(Cow::Owned)),
        }
    }

    /// Map to owned optional element flags
    pub fn map_owned_to_element_flags(maybe_storage_flags: Option<Self>) -> ElementFlags {
        maybe_storage_flags
            .map(|storage_flags| storage_flags.serialize())
            .unwrap_or_default()
    }

    /// Map to optional element flags
    pub fn map_to_some_element_flags(maybe_storage_flags: Option<&Self>) -> Option<ElementFlags> {
        maybe_storage_flags.map(|storage_flags| storage_flags.serialize())
    }

    /// Map to optional element flags
    pub fn map_cow_to_some_element_flags(
        maybe_storage_flags: Option<Cow<Self>>,
    ) -> Option<ElementFlags> {
        maybe_storage_flags.map(|storage_flags| storage_flags.serialize())
    }

    /// Map to optional element flags
    pub fn map_borrowed_cow_to_some_element_flags(
        maybe_storage_flags: &Option<Cow<Self>>,
    ) -> Option<ElementFlags> {
        maybe_storage_flags
            .as_ref()
            .map(|storage_flags| storage_flags.serialize())
    }

    /// Creates optional element flags
    pub fn to_some_element_flags(&self) -> Option<ElementFlags> {
        Some(self.serialize())
    }

    /// Creates element flags.
    pub fn to_element_flags(&self) -> ElementFlags {
        self.serialize()
    }

    /// split_storage_removed_bytes removes bytes as LIFO
    pub fn split_storage_removed_bytes(
        &self,
        removed_key_bytes: u32,
        removed_value_bytes: u32,
    ) -> (StorageRemovedBytes, StorageRemovedBytes) {
        fn single_storage_removal(
            removed_bytes: u32,
            base_epoch: &BaseEpoch,
            owner_id: Option<&OwnerId>,
        ) -> StorageRemovedBytes {
            if removed_bytes == 0 {
                return NoStorageRemoval;
            }
            let bytes_left = removed_bytes;
            let mut sectioned_storage_removal: IntMap<u16, u32> = IntMap::default();
            if bytes_left > 0 {
                // We need to take some from the base epoch
                sectioned_storage_removal.insert(*base_epoch, removed_bytes);
            }
            let mut sectioned_storage_removal_by_identifier: StorageRemovalPerEpochByIdentifier =
                BTreeMap::new();
            if let Some(owner_id) = owner_id {
                sectioned_storage_removal_by_identifier
                    .insert(*owner_id, sectioned_storage_removal);
            } else {
                let default = [0u8; 32];
                sectioned_storage_removal_by_identifier.insert(default, sectioned_storage_removal);
            }
            SectionedStorageRemoval(sectioned_storage_removal_by_identifier)
        }

        fn sectioned_storage_removal(
            removed_bytes: u32,
            base_epoch: &BaseEpoch,
            other_epoch_bytes: &BTreeMap<EpochIndex, BytesAddedInEpoch>,
            owner_id: Option<&OwnerId>,
        ) -> StorageRemovedBytes {
            if removed_bytes == 0 {
                return NoStorageRemoval;
            }
            let mut bytes_left = removed_bytes;
            let mut rev_iter = other_epoch_bytes.iter().rev();
            let mut sectioned_storage_removal: IntMap<u16, u32> = IntMap::default();

            while bytes_left > 0 {
                if let Some((epoch_index, bytes_in_epoch)) = rev_iter.next() {
                    if *bytes_in_epoch <= bytes_left + MINIMUM_NON_BASE_FLAGS_SIZE {
                        sectioned_storage_removal
                            .insert(*epoch_index, *bytes_in_epoch - MINIMUM_NON_BASE_FLAGS_SIZE);
                        bytes_left -= *bytes_in_epoch - MINIMUM_NON_BASE_FLAGS_SIZE;
                    } else {
                        // Correctly take only the required bytes_left from this epoch
                        sectioned_storage_removal.insert(*epoch_index, bytes_left);
                        bytes_left = 0; // All required bytes have been removed, stop processing
                        break; // Exit the loop as there's no need to process
                               // further epochs
                    }
                } else {
                    break;
                }
            }

            if bytes_left > 0 {
                // If there are still bytes left, take them from the base epoch
                sectioned_storage_removal.insert(*base_epoch, bytes_left);
            }

            let mut sectioned_storage_removal_by_identifier: StorageRemovalPerEpochByIdentifier =
                BTreeMap::new();

            if let Some(owner_id) = owner_id {
                sectioned_storage_removal_by_identifier
                    .insert(*owner_id, sectioned_storage_removal);
            } else {
                let default = [0u8; 32];
                sectioned_storage_removal_by_identifier.insert(default, sectioned_storage_removal);
            }

            SectionedStorageRemoval(sectioned_storage_removal_by_identifier)
        }

        // If key bytes are being removed, it implies a delete; thus, we should remove
        // all relevant storage bytes
        let key_storage_removal = if removed_key_bytes > 0 {
            match self {
                // For any variant, always take the key's removed bytes from the base epoch
                SingleEpoch(base_epoch) | MultiEpoch(base_epoch, _) => {
                    single_storage_removal(removed_key_bytes, base_epoch, None)
                }
                SingleEpochOwned(base_epoch, owner_id)
                | MultiEpochOwned(base_epoch, _, owner_id) => {
                    single_storage_removal(removed_key_bytes, base_epoch, Some(owner_id))
                }
            }
        } else {
            StorageRemovedBytes::default()
        };

        // For normal logic, we only need to process the value-related bytes.
        let value_storage_removal = match self {
            SingleEpoch(base_epoch) => {
                single_storage_removal(removed_value_bytes, base_epoch, None)
            }
            SingleEpochOwned(base_epoch, owner_id) => {
                single_storage_removal(removed_value_bytes, base_epoch, Some(owner_id))
            }
            MultiEpoch(base_epoch, other_epoch_bytes) => {
                sectioned_storage_removal(removed_value_bytes, base_epoch, other_epoch_bytes, None)
            }
            MultiEpochOwned(base_epoch, other_epoch_bytes, owner_id) => sectioned_storage_removal(
                removed_value_bytes,
                base_epoch,
                other_epoch_bytes,
                Some(owner_id),
            ),
        };

        // For key removal, simply return the empty removal since it's an update does
        // not modify the key.
        (key_storage_removal, value_storage_removal)
    }

    /// Wrap Storage Flags into optional owned cow
    pub fn into_optional_cow<'a>(self) -> Option<Cow<'a, Self>> {
        Some(Cow::Owned(self))
    }
}

#[cfg(test)]
mod storage_flags_tests {
    use std::collections::BTreeMap;

    use grovedb_costs::storage_cost::removal::{
        StorageRemovalPerEpochByIdentifier, StorageRemovedBytes,
    };
    use intmap::IntMap;

    use crate::{
        BaseEpoch, BytesAddedInEpoch, MergingOwnersStrategy, OwnerId, StorageFlags,
        MINIMUM_NON_BASE_FLAGS_SIZE,
    };
    #[test]
    fn test_storage_flags_combine() {
        {
            // Same SingleEpoch - AdditionBytes
            let common_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::new_single_epoch(common_base_index, None);
            let right_flag = StorageFlags::new_single_epoch(common_base_index, None);

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            // println!(
            //     "{:?} & {:?} added_bytes:{} --> {:?}\n",
            //     left_flag, right_flag, added_bytes, combined_flag
            // );
        }
        // {
        // Same SingleEpoch - RemovedBytes
        // let common_base_index: BaseEpoch = 1;
        // let left_flag = StorageFlags::new_single_epoch(common_base_index, None);
        // let right_flag = StorageFlags::new_single_epoch(common_base_index, None);
        //
        // let removed_bytes = StorageRemovedBytes::BasicStorageRemoval(10);
        // let combined_flag =
        // left_flag.clone().combine_removed_bytes(right_flag.clone(), &removed_bytes,
        // MergingOwnersStrategy::UseOurs); println!("{:?} & {:?}
        // removed_bytes:{:?} --> {:?}\n", left_flag, right_flag, removed_bytes,
        // combined_flag); }
        {
            // Different-Higher SingleEpoch - AdditionBytes
            let left_base_index: BaseEpoch = 1;
            let right_base_index: BaseEpoch = 2;
            let left_flag = StorageFlags::new_single_epoch(left_base_index, None);
            let right_flag = StorageFlags::new_single_epoch(right_base_index, None);

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            // println!(
            //     "{:?} & {:?} added_bytes:{} --> {:?}\n",
            //     left_flag, right_flag, added_bytes, combined_flag
            // );
        }
        {
            // Different-Lesser SingleEpoch - AdditionBytes
            let left_base_index: BaseEpoch = 2;
            let right_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::new_single_epoch(left_base_index, None);
            let right_flag = StorageFlags::new_single_epoch(right_base_index, None);

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            // println!(
            //     "{:?} & {:?} added_bytes:{} --> {:?}\n",
            //     left_flag, right_flag, added_bytes, combined_flag
            // );
        }
        {
            // SingleEpoch-MultiEpoch same BaseEpoch - AdditionBytes
            let common_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::new_single_epoch(common_base_index, None);
            let right_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 5)].iter().cloned().collect(),
            );

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            // println!(
            //     "{:?} & {:?} added_bytes:{} --> {:?}\n",
            //     left_flag, right_flag, added_bytes, combined_flag
            // );
        }
        {
            // SingleEpoch-MultiEpoch higher BaseEpoch - AdditionBytes
            let left_base_index: BaseEpoch = 1;
            let right_base_index: BaseEpoch = 2;
            let left_flag = StorageFlags::new_single_epoch(left_base_index, None);
            let right_flag = StorageFlags::MultiEpoch(
                right_base_index,
                [(right_base_index + 1, 5)].iter().cloned().collect(),
            );

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            // println!(
            //     "{:?} & {:?} added_bytes:{} --> {:?}\n",
            //     left_flag, right_flag, added_bytes, combined_flag
            // );
        }
        // {
        // SingleEpoch-MultiEpoch same BaseEpoch - RemovedBytes (positive difference)
        // let common_base_index: BaseEpoch = 1;
        // let left_flag = StorageFlags::new_single_epoch(common_base_index, None);
        // let right_flag = StorageFlags::MultiEpoch(common_base_index,
        // [(common_base_index + 1, 10)].iter().cloned().collect());
        //
        // let removed_bytes = StorageRemovedBytes::BasicStorageRemoval(3);
        // let combined_flag =
        // left_flag.clone().combine_removed_bytes(right_flag.clone(), &removed_bytes,
        // MergingOwnersStrategy::UseOurs); println!("{:?} & {:?}
        // removed_bytes:{:?} --> {:?}\n", left_flag, right_flag, &removed_bytes,
        // combined_flag); }
        // {
        // SingleEpoch-MultiEpoch same BaseEpoch - RemovedBytes (negative difference)
        // let common_base_index: BaseEpoch = 1;
        // let left_flag = StorageFlags::new_single_epoch(common_base_index, None);
        // let right_flag = StorageFlags::MultiEpoch(common_base_index,
        // [(common_base_index + 1, 10)].iter().cloned().collect());
        //
        // let removed_bytes = StorageRemovedBytes::BasicStorageRemoval(13);
        // let combined_flag =
        // left_flag.clone().combine_removed_bytes(right_flag.clone(), &removed_bytes,
        // MergingOwnersStrategy::UseOurs); println!("{:?} & {:?}
        // removed_bytes:{:?} --> {:?}\n", left_flag, right_flag, &removed_bytes,
        // combined_flag); }
        // {
        // SingleEpoch-MultiEpoch higher BaseEpoch - RemovedBytes (positive difference)
        // let left_base_index: BaseEpoch = 1;
        // let right_base_index: BaseEpoch = 2;
        // let left_flag = StorageFlags::new_single_epoch(left_base_index, None);
        // let right_flag = StorageFlags::MultiEpoch(right_base_index,
        // [(right_base_index + 1, 10)].iter().cloned().collect());
        //
        // let removed_bytes = StorageRemovedBytes::BasicStorageRemoval(3);
        // let combined_flag =
        // left_flag.clone().combine_removed_bytes(right_flag.clone(), &removed_bytes,
        // MergingOwnersStrategy::UseOurs); println!("{:?} & {:?}
        // removed_bytes:{:?} --> {:?}\n", left_flag, right_flag, &removed_bytes,
        // combined_flag); }
        // {
        // SingleEpoch-MultiEpoch higher BaseEpoch - RemovedBytes (negative difference)
        // let left_base_index: BaseEpoch = 1;
        // let right_base_index: BaseEpoch = 2;
        // let left_flag = StorageFlags::new_single_epoch(left_base_index, None);
        // let right_flag = StorageFlags::MultiEpoch(right_base_index,
        // [(right_base_index + 1, 5)].iter().cloned().collect());
        //
        // let removed_bytes = StorageRemovedBytes::BasicStorageRemoval(7);
        // let combined_flag =
        // left_flag.clone().combine_removed_bytes(right_flag.clone(), &removed_bytes,
        // MergingOwnersStrategy::UseOurs); println!("{:?} & {:?}
        // removed_bytes:{:?} --> {:?}\n", left_flag, right_flag, &removed_bytes,
        // combined_flag); }
        {
            // MultiEpochs same BaseEpoch - AdditionBytes #1
            let common_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 7)].iter().cloned().collect(),
            );
            let right_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 5)].iter().cloned().collect(),
            );

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            println!(
                "{:?} & {:?} added_bytes:{} --> {:?}\n",
                left_flag, right_flag, added_bytes, combined_flag
            );
        }
        {
            // MultiEpochs same BaseEpoch - AdditionBytes #2
            let common_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 7)].iter().cloned().collect(),
            );
            let right_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 2, 5)].iter().cloned().collect(),
            );

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            println!(
                "{:?} & {:?} added_bytes:{} --> {:?}\n",
                left_flag, right_flag, added_bytes, combined_flag
            );
        }
        {
            // MultiEpochs same BaseEpoch - AdditionBytes #3
            let common_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 7)].iter().cloned().collect(),
            );
            let right_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 3), (common_base_index + 2, 5)]
                    .iter()
                    .cloned()
                    .collect(),
            );

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            println!(
                "{:?} & {:?} added_bytes:{} --> {:?}\n",
                left_flag, right_flag, added_bytes, combined_flag
            );
        }
        {
            // MultiEpochs higher BaseEpoch - AdditionBytes #1
            let common_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 7)].iter().cloned().collect(),
            );
            let right_flag = StorageFlags::MultiEpoch(
                common_base_index + 1,
                [(common_base_index + 1, 5)].iter().cloned().collect(),
            );

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            println!(
                "{:?} & {:?} added_bytes:{} --> {:?}\n",
                left_flag, right_flag, added_bytes, combined_flag
            );
        }
        {
            // MultiEpochs higher BaseEpoch - AdditionBytes #2
            let common_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 7)].iter().cloned().collect(),
            );
            let right_flag = StorageFlags::MultiEpoch(
                common_base_index + 1,
                [(common_base_index + 2, 5)].iter().cloned().collect(),
            );

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            println!(
                "{:?} & {:?} added_bytes:{} --> {:?}\n",
                left_flag, right_flag, added_bytes, combined_flag
            );
        }
        {
            // MultiEpochs higher BaseEpoch - AdditionBytes #3
            let common_base_index: BaseEpoch = 1;
            let left_flag = StorageFlags::MultiEpoch(
                common_base_index,
                [(common_base_index + 1, 7)].iter().cloned().collect(),
            );
            let right_flag = StorageFlags::MultiEpoch(
                common_base_index + 1,
                [(common_base_index + 2, 3), (common_base_index + 3, 5)]
                    .iter()
                    .cloned()
                    .collect(),
            );

            let added_bytes: BytesAddedInEpoch = 10;
            let combined_flag = left_flag.clone().combine_added_bytes(
                right_flag.clone(),
                added_bytes,
                MergingOwnersStrategy::UseOurs,
            );
            println!(
                "{:?} & {:?} added_bytes:{} --> {:?}\n",
                left_flag, right_flag, added_bytes, combined_flag
            );
        }
    }

    fn create_epoch_map(epoch: u16, bytes: u32) -> BTreeMap<u16, u32> {
        let mut map = BTreeMap::new();
        map.insert(epoch, bytes);
        map
    }

    fn default_owner_id() -> OwnerId {
        [0u8; 32]
    }

    fn single_epoch_removed_bytes_map(
        owner_id: [u8; 32],
        epoch_index: u16,
        bytes_removed: u32,
    ) -> StorageRemovalPerEpochByIdentifier {
        let mut removed_bytes = StorageRemovalPerEpochByIdentifier::default();
        let mut removed_bytes_for_identity = IntMap::new();
        removed_bytes_for_identity.insert(epoch_index, bytes_removed);
        removed_bytes.insert(owner_id, removed_bytes_for_identity);
        removed_bytes
    }

    fn multi_epoch_removed_bytes_map(
        owner_id: [u8; 32],
        removed_bytes_per_epoch: IntMap<u16, u32>,
    ) -> StorageRemovalPerEpochByIdentifier {
        let mut removed_bytes = StorageRemovalPerEpochByIdentifier::default();
        removed_bytes.insert(owner_id, removed_bytes_per_epoch);
        removed_bytes
    }

    #[test]
    fn test_combine_that_would_remove_the_epoch_completely() {
        let owner_id = [2; 32];
        let left_base_index: BaseEpoch = 0;
        let right_base_index: BaseEpoch = 1;
        let other_epochs = create_epoch_map(1, 5);
        let left_flag = StorageFlags::MultiEpochOwned(left_base_index, other_epochs, owner_id);
        let right_flag = StorageFlags::new_single_epoch(right_base_index, Some(owner_id));

        let removed_bytes = single_epoch_removed_bytes_map(owner_id, 1, 3);
        let combined_flag = left_flag
            .clone()
            .combine_removed_bytes(
                right_flag.clone(),
                &StorageRemovedBytes::SectionedStorageRemoval(removed_bytes.clone()),
                MergingOwnersStrategy::UseOurs,
            )
            .expect("expected to combine flags");

        assert_eq!(
            combined_flag,
            StorageFlags::SingleEpochOwned(left_base_index, owner_id)
        );
        println!(
            "{:?} & {:?} removed:{:?} --> {:?}\n",
            left_flag, right_flag, removed_bytes, combined_flag
        );
    }

    #[test]
    fn test_combine_that_would_remove_the_epoch_completely_with_many_entries() {
        let owner_id = [2; 32];
        let left_base_index: BaseEpoch = 0;
        let right_base_index: BaseEpoch = 1;
        let mut other_epochs = BTreeMap::new();
        let mut removed_bytes = IntMap::new();
        for i in 1..200 {
            other_epochs.insert(i, MINIMUM_NON_BASE_FLAGS_SIZE + 1);
            removed_bytes.insert(i, 1); // anything between 1 and
                                        // MINIMUM_NON_BASE_FLAGS_SIZE +
                                        // 1 would be the same
        }

        let left_flag = StorageFlags::MultiEpochOwned(left_base_index, other_epochs, owner_id);
        let right_flag = StorageFlags::new_single_epoch(right_base_index, Some(owner_id));

        let removed_bytes = multi_epoch_removed_bytes_map(owner_id, removed_bytes);
        let combined_flag = left_flag
            .clone()
            .combine_removed_bytes(
                right_flag.clone(),
                &StorageRemovedBytes::SectionedStorageRemoval(removed_bytes.clone()),
                MergingOwnersStrategy::UseOurs,
            )
            .expect("expected to combine flags");

        assert_eq!(
            combined_flag,
            StorageFlags::SingleEpochOwned(left_base_index, owner_id)
        );
        println!(
            "{:?} & {:?} removed:{:?} --> {:?}\n",
            left_flag, right_flag, removed_bytes, combined_flag
        );
    }

    /// Tests the case when using SingleEpoch flags, ensuring that the correct
    /// storage removal is calculated.
    #[test]
    fn test_single_epoch_removal() {
        let flags = StorageFlags::SingleEpoch(5);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(100, 200);

        assert_eq!(
            key_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(default_owner_id(), IntMap::from_iter([(5u16, 100)]));
                map
            })
        );
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(default_owner_id(), IntMap::from_iter([(5u16, 200)]));
                map
            })
        );
    }

    /// Tests SingleEpochOwned flags where the removal is done under an OwnerId
    #[test]
    fn test_single_epoch_owned_removal() {
        let owner_id = [1u8; 32];
        let flags = StorageFlags::SingleEpochOwned(5, owner_id);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(50, 150);

        assert_eq!(
            key_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(owner_id, IntMap::from_iter([(5u16, 50)]));
                map
            })
        );
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(owner_id, IntMap::from_iter([(5u16, 150)]));
                map
            })
        );
    }

    /// Tests the case where multiple epochs are used and the total removal
    /// doesn’t exceed the extra epoch bytes
    #[test]
    fn test_multi_epoch_removal_no_remaining_base() {
        let mut other_epochs = create_epoch_map(6, 100);
        other_epochs.insert(7, 200);

        let flags = StorageFlags::MultiEpoch(5, other_epochs);
        let (_key_removal, value_removal) = flags.split_storage_removed_bytes(0, 250);

        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(
                    default_owner_id(),
                    IntMap::from_iter([(7u16, 197), (6u16, 53)]),
                );
                map
            })
        );
    }

    /// Similar to the previous test, but this time the base epoch is also used
    /// due to insufficient bytes in the extra epochs
    #[test]
    fn test_multi_epoch_removal_with_remaining_base() {
        let mut other_epochs = create_epoch_map(6, 100);
        other_epochs.insert(7, 50);

        let flags = StorageFlags::MultiEpoch(5, other_epochs);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(250, 250);

        assert_eq!(
            key_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(default_owner_id(), IntMap::from_iter([(5u16, 250)]));
                map
            })
        );
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(
                    default_owner_id(),
                    IntMap::from_iter([(7u16, 47), (6u16, 97), (5u16, 106)]),
                );
                map
            })
        );
    }

    /// Same as last test but for owned flags with OwnerId
    #[test]
    fn test_multi_epoch_owned_removal_with_remaining_base() {
        let owner_id = [2u8; 32];
        let mut other_epochs = create_epoch_map(6, 100);
        other_epochs.insert(7, 50);

        let flags = StorageFlags::MultiEpochOwned(5, other_epochs, owner_id);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(250, 250);

        assert_eq!(
            key_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(owner_id, IntMap::from_iter([(5u16, 250)]));
                map
            })
        );
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(
                    owner_id,
                    IntMap::from_iter([(7u16, 47), (6u16, 97), (5u16, 106)]),
                );
                map
            })
        );
    }

    /// Tests the function when zero bytes are to be removed, expecting an empty
    /// removal result
    #[test]
    fn test_single_epoch_removal_zero_bytes() {
        let flags = StorageFlags::SingleEpoch(5);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(0, 0);

        assert_eq!(key_removal, StorageRemovedBytes::NoStorageRemoval);
        assert_eq!(value_removal, StorageRemovedBytes::NoStorageRemoval);
    }

    /// Tests the removal of only part of the bytes using SingleEpochOwned
    #[test]
    fn test_single_epoch_owned_removal_partial_bytes() {
        let owner_id = [3u8; 32];
        let flags = StorageFlags::SingleEpochOwned(5, owner_id);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(100, 50);

        assert_eq!(
            key_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(owner_id, IntMap::from_iter([(5u16, 100)]));
                map
            })
        );
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(owner_id, IntMap::from_iter([(5u16, 50)]));
                map
            })
        );
    }

    /// Ensures that the function correctly handles when there are more bytes to
    /// be removed than are available in the epoch map, requiring the base epoch
    /// to be used
    #[test]
    fn test_multi_epoch_removal_excess_bytes() {
        let mut other_epochs = create_epoch_map(6, 100);
        other_epochs.insert(7, 200);

        let flags = StorageFlags::MultiEpoch(5, other_epochs);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(400, 300);

        assert_eq!(
            key_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(default_owner_id(), IntMap::from_iter([(5u16, 400)]));
                map
            })
        );
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(
                    default_owner_id(),
                    IntMap::from_iter([(7u16, 197), (6u16, 97), (5u16, 6)]),
                );
                map
            })
        );
    }

    /// Similar to the previous test, but for owned flags with OwnerId
    #[test]
    fn test_multi_epoch_owned_removal_excess_bytes() {
        let owner_id = [4u8; 32];
        let mut other_epochs = create_epoch_map(6, 100);
        other_epochs.insert(7, 200);

        let flags = StorageFlags::MultiEpochOwned(5, other_epochs, owner_id);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(450, 350);

        assert_eq!(
            key_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(owner_id, IntMap::from_iter([(5u16, 450)]));
                map
            })
        );
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(
                    owner_id,
                    IntMap::from_iter([(7u16, 197), (6u16, 97), (5u16, 56)]),
                );
                map
            })
        );
    }

    #[test]
    /// This test verifies the `split_storage_removed_bytes` function when all
    /// required bytes are taken from non-base epochs during the removal
    /// process.
    ///
    /// The scenario:
    /// - The test initializes a `StorageFlags::MultiEpochOwned` with a
    ///   `BaseEpoch` of 5.
    /// - Two additional epochs, 6 and 7, are provided with 300 and 400 bytes
    ///   respectively.
    /// - The function is then called to remove 700 bytes from the value, while
    ///   no bytes are removed from the key.
    ///
    /// The expected behavior:
    /// - For key removal: No bytes should be removed since the key removal
    ///   request is zero.
    /// - For value removal: It should consume all 400 bytes from epoch 7 (LIFO
    ///   order) and the remaining 300 bytes from epoch 6.
    fn test_multi_epoch_owned_removal_all_bytes_taken_from_non_base_epoch() {
        // Define the owner ID as a 32-byte array filled with 5s.
        let owner_id = [5u8; 32];

        // Create a map for additional epochs with 300 bytes in epoch 6.
        let mut other_epochs = create_epoch_map(6, 300);

        // Insert 400 bytes for epoch 7 into the map.
        other_epochs.insert(7, 400);

        // Initialize the `StorageFlags::MultiEpochOwned` with base epoch 5, additional
        // epochs, and the owner ID.
        let flags = StorageFlags::MultiEpochOwned(5, other_epochs, owner_id);

        // Call the function to split the storage removal bytes, expecting to remove 700
        // bytes from the value.
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(0, 700);

        // Verify that no bytes are removed from the key.
        assert_eq!(key_removal, StorageRemovedBytes::NoStorageRemoval);

        // Verify that 700 bytes are removed from the value, consuming 400 bytes from
        // epoch 7 and 300 bytes from epoch 6.
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(
                    owner_id,
                    IntMap::from_iter([(5u16, 6), (6u16, 297), (7u16, 397)]),
                );
                map
            })
        );
    }

    #[test]
    fn test_multi_epoch_removal_remaining_base_epoch() {
        let mut other_epochs = create_epoch_map(6, 300);
        other_epochs.insert(7, 100);

        let flags = StorageFlags::MultiEpoch(5, other_epochs);
        let (key_removal, value_removal) = flags.split_storage_removed_bytes(400, 500);

        assert_eq!(
            key_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(default_owner_id(), IntMap::from_iter([(5u16, 400)]));
                map
            })
        );
        assert_eq!(
            value_removal,
            StorageRemovedBytes::SectionedStorageRemoval({
                let mut map = BTreeMap::new();
                map.insert(
                    default_owner_id(),
                    IntMap::from_iter([(7u16, 97), (6u16, 297), (5u16, 106)]),
                );
                map
            })
        );
    }
}
