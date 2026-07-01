pub mod intersect;
#[cfg(any(feature = "minimal", feature = "verify"))]
mod merge;

use std::{
    cmp,
    cmp::Ordering,
    fmt,
    hash::Hash,
    ops::{Range, RangeFrom, RangeFull, RangeInclusive, RangeTo, RangeToInclusive},
};

use bincode::{enc::write::Writer, error::DecodeError, BorrowDecode, Decode, Encode};
#[cfg(feature = "minimal")]
use grovedb_costs::{CostContext, CostsExt, OperationCost};
#[cfg(feature = "minimal")]
use grovedb_storage::RawIterator;
#[cfg(feature = "serde")]
use serde::de::VariantAccess;
#[cfg(feature = "serde")]
use serde::{Deserialize, Deserializer, Serialize, Serializer};

#[cfg(any(feature = "minimal", feature = "verify"))]
use crate::error::Error;
use crate::proofs::hex_to_ascii;

/// A `QueryItem` represents a key or a range of keys to be included in a proof.
///
/// This enum allows specifying different ways of selecting keys, including
/// exact matches, open-ended ranges, and boundary-based selections.
///
/// # Variants:
/// - `Key(Vec<u8>)` → A specific key.
/// - `Range(Range<Vec<u8>>)` → A range of keys (exclusive upper bound).
/// - `RangeInclusive(RangeInclusive<Vec<u8>>)` → A range of keys (inclusive
///   upper bound).
/// - `RangeFull(RangeFull)` → A full range, including all keys.
/// - `RangeFrom(RangeFrom<Vec<u8>>)` → A range starting from a key (inclusive).
/// - `RangeTo(RangeTo<Vec<u8>>)` → A range up to a key (exclusive).
/// - `RangeToInclusive(RangeToInclusive<Vec<u8>>)` → A range up to a key
///   (inclusive).
/// - `RangeAfter(RangeFrom<Vec<u8>>)` → A range starting after a key
///   (exclusive).
/// - `RangeAfterTo(Range<Vec<u8>>)` → A range between two keys, starting after
///   the lower bound.
/// - `RangeAfterToInclusive(RangeInclusive<Vec<u8>>)` → A range between two
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Clone, Debug, Hash, Eq, PartialEq)]
pub enum QueryItem {
    /// A specific key to be included in the proof.
    Key(Vec<u8>),

    /// A range of keys, **excluding** the upper bound (`start..end`).
    Range(Range<Vec<u8>>),

    /// A range of keys, **including** the upper bound (`start..=end`).
    RangeInclusive(RangeInclusive<Vec<u8>>),

    /// Represents a **full range**, covering **all** possible keys.
    RangeFull(RangeFull),

    /// A range starting **from** a key, **inclusive** (`start..`).
    RangeFrom(RangeFrom<Vec<u8>>),

    /// A range **up to** a key, **exclusive** (`..end`).
    RangeTo(RangeTo<Vec<u8>>),

    /// A range **up to** a key, **inclusive** (`..=end`).
    RangeToInclusive(RangeToInclusive<Vec<u8>>),

    /// A range starting **after** a specific key, **exclusive** (`(key, ∞)`).
    RangeAfter(RangeFrom<Vec<u8>>),

    /// A range starting **after** a key and extending to another key,
    /// **exclusive**.
    RangeAfterTo(Range<Vec<u8>>),

    /// A range starting **after** a key and extending to another key,
    /// **inclusive**.
    RangeAfterToInclusive(RangeInclusive<Vec<u8>>),
}

#[cfg(feature = "serde")]
impl Serialize for QueryItem {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            QueryItem::Key(key) => serializer.serialize_newtype_variant("QueryItem", 0, "Key", key),
            QueryItem::Range(range) => {
                serializer.serialize_newtype_variant("QueryItem", 1, "Range", &range)
            }
            QueryItem::RangeInclusive(range) => {
                serializer.serialize_newtype_variant("QueryItem", 2, "RangeInclusive", range)
            }
            QueryItem::RangeFull(_) => {
                serializer.serialize_unit_variant("QueryItem", 3, "RangeFull")
            }
            QueryItem::RangeFrom(range_from) => {
                serializer.serialize_newtype_variant("QueryItem", 4, "RangeFrom", range_from)
            }
            QueryItem::RangeTo(range_to) => {
                serializer.serialize_newtype_variant("QueryItem", 5, "RangeTo", range_to)
            }
            QueryItem::RangeToInclusive(range_to_inclusive) => serializer
                .serialize_newtype_variant(
                    "QueryItem",
                    6,
                    "RangeToInclusive",
                    &range_to_inclusive.end,
                ),
            QueryItem::RangeAfter(range_after) => {
                serializer.serialize_newtype_variant("QueryItem", 7, "RangeAfter", range_after)
            }
            QueryItem::RangeAfterTo(range_after_to) => {
                serializer.serialize_newtype_variant("QueryItem", 8, "RangeAfterTo", range_after_to)
            }
            QueryItem::RangeAfterToInclusive(range_after_to_inclusive) => serializer
                .serialize_newtype_variant(
                    "QueryItem",
                    9,
                    "RangeAfterToInclusive",
                    range_after_to_inclusive,
                ),
        }
    }
}

#[cfg(feature = "serde")]
impl<'de> Deserialize<'de> for QueryItem {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        #[derive(Deserialize)]
        #[serde(field_identifier, rename_all = "snake_case")]
        enum Field {
            Key,
            Range,
            RangeInclusive,
            RangeFull,
            RangeFrom,
            RangeTo,
            RangeToInclusive,
            RangeAfter,
            RangeAfterTo,
            RangeAfterToInclusive,
        }

        struct QueryItemVisitor;

        impl<'de> serde::de::Visitor<'de> for QueryItemVisitor {
            type Value = QueryItem;

            fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result {
                formatter.write_str("enum QueryItem")
            }

            fn visit_enum<A>(self, data: A) -> Result<Self::Value, A::Error>
            where
                A: serde::de::EnumAccess<'de>,
            {
                let (variant, variant_access) = data.variant()?;

                match variant {
                    Field::Key => {
                        let key = variant_access.newtype_variant()?;
                        Ok(QueryItem::Key(key))
                    }
                    Field::Range => {
                        let range = variant_access.newtype_variant()?;
                        Ok(QueryItem::Range(range))
                    }
                    Field::RangeInclusive => {
                        let range_inclusive = variant_access.newtype_variant()?;
                        Ok(QueryItem::RangeInclusive(range_inclusive))
                    }
                    Field::RangeFull => Ok(QueryItem::RangeFull(RangeFull)),
                    Field::RangeFrom => {
                        let range_from = variant_access.newtype_variant()?;
                        Ok(QueryItem::RangeFrom(range_from))
                    }
                    Field::RangeTo => {
                        let range_to = variant_access.newtype_variant()?;
                        Ok(QueryItem::RangeTo(range_to))
                    }
                    Field::RangeToInclusive => {
                        // Deserialize the `Vec<u8>` for the `end` of the range
                        let end = variant_access.newtype_variant()?;
                        Ok(QueryItem::RangeToInclusive(..=end))
                    }
                    Field::RangeAfter => {
                        let range_after = variant_access.newtype_variant()?;
                        Ok(QueryItem::RangeAfter(range_after))
                    }
                    Field::RangeAfterTo => {
                        let range_after_to = variant_access.newtype_variant()?;
                        Ok(QueryItem::RangeAfterTo(range_after_to))
                    }
                    Field::RangeAfterToInclusive => {
                        let range_after_to_inclusive = variant_access.newtype_variant()?;
                        Ok(QueryItem::RangeAfterToInclusive(range_after_to_inclusive))
                    }
                }
            }
        }

        const VARIANTS: &[&str] = &[
            "Key",
            "Range",
            "RangeInclusive",
            "RangeFull",
            "RangeFrom",
            "RangeTo",
            "RangeToInclusive",
            "RangeAfter",
            "RangeAfterTo",
            "RangeAfterToInclusive",
        ];

        deserializer.deserialize_enum("QueryItem", VARIANTS, QueryItemVisitor)
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Encode for QueryItem {
    fn encode<E: bincode::enc::Encoder>(
        &self,
        encoder: &mut E,
    ) -> Result<(), bincode::error::EncodeError> {
        match self {
            QueryItem::Key(key) => {
                encoder.writer().write(&[0])?;
                key.encode(encoder)
            }
            QueryItem::Range(range) => {
                encoder.writer().write(&[1])?;
                range.start.encode(encoder)?;
                range.end.encode(encoder)
            }
            QueryItem::RangeInclusive(range) => {
                encoder.writer().write(&[2])?;
                range.start().encode(encoder)?;
                range.end().encode(encoder)
            }
            QueryItem::RangeFull(_) => {
                encoder.writer().write(&[3])?;
                Ok(())
            }
            QueryItem::RangeFrom(range) => {
                encoder.writer().write(&[4])?;
                range.start.encode(encoder)
            }
            QueryItem::RangeTo(range) => {
                encoder.writer().write(&[5])?;
                range.end.encode(encoder)
            }
            QueryItem::RangeToInclusive(range) => {
                encoder.writer().write(&[6])?;
                range.end.encode(encoder)
            }
            QueryItem::RangeAfter(range) => {
                encoder.writer().write(&[7])?;
                range.start.encode(encoder)
            }
            QueryItem::RangeAfterTo(range) => {
                encoder.writer().write(&[8])?;
                range.start.encode(encoder)?;
                range.end.encode(encoder)
            }
            QueryItem::RangeAfterToInclusive(range) => {
                encoder.writer().write(&[9])?;
                range.start().encode(encoder)?;
                range.end().encode(encoder)
            }
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Decode for QueryItem {
    fn decode<D: bincode::de::Decoder>(decoder: &mut D) -> Result<Self, DecodeError> {
        let variant_id = u8::decode(decoder)?;

        match variant_id {
            0 => {
                let key = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::Key(key))
            }
            1 => {
                let start = Vec::<u8>::decode(decoder)?;
                let end = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::Range(start..end))
            }
            2 => {
                let start = Vec::<u8>::decode(decoder)?;
                let end = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::RangeInclusive(start..=end))
            }
            3 => Ok(QueryItem::RangeFull(RangeFull)),
            4 => {
                let start = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::RangeFrom(start..))
            }
            5 => {
                let end = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::RangeTo(..end))
            }
            6 => {
                let end = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::RangeToInclusive(..=end))
            }
            7 => {
                let start = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::RangeAfter(start..))
            }
            8 => {
                let start = Vec::<u8>::decode(decoder)?;
                let end = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::RangeAfterTo(start..end))
            }
            9 => {
                let start = Vec::<u8>::decode(decoder)?;
                let end = Vec::<u8>::decode(decoder)?;
                Ok(QueryItem::RangeAfterToInclusive(start..=end))
            }
            _ => Err(DecodeError::UnexpectedVariant {
                type_name: "QueryItem",
                allowed: &bincode::error::AllowedEnumVariants::Range { min: 0, max: 9 },
                found: variant_id as u32,
            }),
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl<'de> BorrowDecode<'de> for QueryItem {
    fn borrow_decode<D: bincode::de::BorrowDecoder<'de>>(
        decoder: &mut D,
    ) -> Result<Self, DecodeError> {
        let variant_id = u8::decode(decoder)?;

        match variant_id {
            0 => {
                let key = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::Key(key))
            }
            1 => {
                let start = Vec::<u8>::borrow_decode(decoder)?;
                let end = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::Range(start..end))
            }
            2 => {
                let start = Vec::<u8>::borrow_decode(decoder)?;
                let end = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::RangeInclusive(start..=end))
            }
            3 => Ok(QueryItem::RangeFull(RangeFull)),
            4 => {
                let start = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::RangeFrom(start..))
            }
            5 => {
                let end = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::RangeTo(..end))
            }
            6 => {
                let end = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::RangeToInclusive(..=end))
            }
            7 => {
                let start = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::RangeAfter(start..))
            }
            8 => {
                let start = Vec::<u8>::borrow_decode(decoder)?;
                let end = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::RangeAfterTo(start..end))
            }
            9 => {
                let start = Vec::<u8>::borrow_decode(decoder)?;
                let end = Vec::<u8>::borrow_decode(decoder)?;
                Ok(QueryItem::RangeAfterToInclusive(start..=end))
            }
            _ => Err(DecodeError::UnexpectedVariant {
                type_name: "QueryItem",
                allowed: &bincode::error::AllowedEnumVariants::Range { min: 0, max: 9 },
                found: variant_id as u32,
            }),
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl fmt::Display for QueryItem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            QueryItem::Key(key) => write!(f, "Key({})", hex_to_ascii(key)),
            QueryItem::Range(range) => write!(
                f,
                "Range({} .. {})",
                hex_to_ascii(&range.start),
                hex_to_ascii(&range.end)
            ),
            QueryItem::RangeInclusive(range) => write!(
                f,
                "RangeInclusive({} ..= {})",
                hex_to_ascii(range.start()),
                hex_to_ascii(range.end())
            ),
            QueryItem::RangeFull(_) => write!(f, "RangeFull"),
            QueryItem::RangeFrom(range) => {
                write!(f, "RangeFrom({} ..)", hex_to_ascii(&range.start))
            }
            QueryItem::RangeTo(range) => write!(f, "RangeTo(.. {})", hex_to_ascii(&range.end)),
            QueryItem::RangeToInclusive(range) => {
                write!(f, "RangeToInclusive(..= {})", hex_to_ascii(&range.end))
            }
            QueryItem::RangeAfter(range) => {
                write!(f, "RangeAfter({} <..)", hex_to_ascii(&range.start))
            }
            QueryItem::RangeAfterTo(range) => write!(
                f,
                "RangeAfterTo({} <.. {})",
                hex_to_ascii(&range.start),
                hex_to_ascii(&range.end)
            ),
            QueryItem::RangeAfterToInclusive(range) => write!(
                f,
                "RangeAfterToInclusive({} <..= {})",
                hex_to_ascii(range.start()),
                hex_to_ascii(range.end())
            ),
        }
    }
}

impl QueryItem {
    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub fn processing_footprint(&self) -> u32 {
        match self {
            QueryItem::Key(key) => key.len() as u32,
            QueryItem::RangeFull(_) => 0u32,
            _ => {
                self.lower_bound().0.map_or(0u32, |x| x.len() as u32)
                    + self.upper_bound().0.map_or(0u32, |x| x.len() as u32)
            }
        }
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub fn lower_bound(&self) -> (Option<&[u8]>, bool) {
        match self {
            QueryItem::Key(key) => (Some(key.as_slice()), false),
            QueryItem::Range(range) => (Some(range.start.as_ref()), false),
            QueryItem::RangeInclusive(range) => (Some(range.start().as_ref()), false),
            QueryItem::RangeFull(_) => (None, false),
            QueryItem::RangeFrom(range) => (Some(range.start.as_ref()), false),
            QueryItem::RangeTo(_) => (None, false),
            QueryItem::RangeToInclusive(_) => (None, false),
            QueryItem::RangeAfter(range) => (Some(range.start.as_ref()), true),
            QueryItem::RangeAfterTo(range) => (Some(range.start.as_ref()), true),
            QueryItem::RangeAfterToInclusive(range) => (Some(range.start().as_ref()), true),
        }
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub const fn lower_unbounded(&self) -> bool {
        match self {
            QueryItem::Key(_) => false,
            QueryItem::Range(_) => false,
            QueryItem::RangeInclusive(_) => false,
            QueryItem::RangeFull(_) => true,
            QueryItem::RangeFrom(_) => false,
            QueryItem::RangeTo(_) => true,
            QueryItem::RangeToInclusive(_) => true,
            QueryItem::RangeAfter(_) => false,
            QueryItem::RangeAfterTo(_) => false,
            QueryItem::RangeAfterToInclusive(_) => false,
        }
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub fn upper_bound(&self) -> (Option<&[u8]>, bool) {
        match self {
            QueryItem::Key(key) => (Some(key.as_slice()), true),
            QueryItem::Range(range) => (Some(range.end.as_ref()), false),
            QueryItem::RangeInclusive(range) => (Some(range.end().as_ref()), true),
            QueryItem::RangeFull(_) => (None, true),
            QueryItem::RangeFrom(_) => (None, true),
            QueryItem::RangeTo(range) => (Some(range.end.as_ref()), false),
            QueryItem::RangeToInclusive(range) => (Some(range.end.as_ref()), true),
            QueryItem::RangeAfter(_) => (None, true),
            QueryItem::RangeAfterTo(range) => (Some(range.end.as_ref()), false),
            QueryItem::RangeAfterToInclusive(range) => (Some(range.end().as_ref()), true),
        }
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub const fn upper_unbounded(&self) -> bool {
        match self {
            QueryItem::Key(_) => false,
            QueryItem::Range(_) => false,
            QueryItem::RangeInclusive(_) => false,
            QueryItem::RangeFull(_) => true,
            QueryItem::RangeFrom(_) => true,
            QueryItem::RangeTo(_) => false,
            QueryItem::RangeToInclusive(_) => false,
            QueryItem::RangeAfter(_) => true,
            QueryItem::RangeAfterTo(_) => false,
            QueryItem::RangeAfterToInclusive(_) => false,
        }
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub fn contains(&self, key: &[u8]) -> bool {
        let (lower_bound, lower_bound_non_inclusive) = self.lower_bound();
        let (upper_bound, upper_bound_inclusive) = self.upper_bound();
        (self.lower_unbounded()
            || Some(key) > lower_bound
            || (Some(key) == lower_bound && !lower_bound_non_inclusive))
            && (self.upper_unbounded()
                || Some(key) < upper_bound
                || (Some(key) == upper_bound && upper_bound_inclusive))
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    fn enum_value(&self) -> u32 {
        match self {
            QueryItem::Key(_) => 0,
            QueryItem::Range(_) => 1,
            QueryItem::RangeInclusive(_) => 2,
            QueryItem::RangeFull(_) => 3,
            QueryItem::RangeFrom(_) => 4,
            QueryItem::RangeTo(_) => 5,
            QueryItem::RangeToInclusive(_) => 6,
            QueryItem::RangeAfter(_) => 7,
            QueryItem::RangeAfterTo(_) => 8,
            QueryItem::RangeAfterToInclusive(_) => 9,
        }
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub const fn is_key(&self) -> bool {
        matches!(self, QueryItem::Key(_))
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub const fn is_range(&self) -> bool {
        matches!(
            self,
            QueryItem::Range(_)
                | QueryItem::RangeInclusive(_)
                | QueryItem::RangeFull(_)
                | QueryItem::RangeFrom(_)
                | QueryItem::RangeTo(_)
                | QueryItem::RangeToInclusive(_)
                | QueryItem::RangeAfter(_)
                | QueryItem::RangeAfterTo(_)
                | QueryItem::RangeAfterToInclusive(_)
        )
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub const fn is_single(&self) -> bool {
        matches!(self, QueryItem::Key(_))
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub const fn is_unbounded_range(&self) -> bool {
        !matches!(
            self,
            QueryItem::Key(_) | QueryItem::Range(_) | QueryItem::RangeInclusive(_)
        )
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub fn keys(&self) -> Result<Vec<Vec<u8>>, Error> {
        match self {
            QueryItem::Key(key) => Ok(vec![key.clone()]),
            QueryItem::Range(Range { start, end }) => {
                let mut keys = vec![];
                if start.len() > 1 || end.len() != 1 {
                    return Err(Error::InvalidOperation(
                        "distinct keys are not available for ranges using more or less than 1 byte",
                    ));
                }
                let start = *start.first().unwrap_or_else(|| {
                    keys.push(vec![]);
                    &0
                });
                if let Some(end) = end.first() {
                    let end = *end;
                    for i in start..end {
                        keys.push(vec![i]);
                    }
                }
                Ok(keys)
            }
            QueryItem::RangeInclusive(range_inclusive) => {
                let start = range_inclusive.start();
                let end = range_inclusive.end();
                let mut keys = vec![];
                if start.len() > 1 || end.len() != 1 {
                    return Err(Error::InvalidOperation(
                        "distinct keys are not available for ranges using more or less than 1 byte",
                    ));
                }
                let start = *start.first().unwrap_or_else(|| {
                    keys.push(vec![]);
                    &0
                });
                if let Some(end) = end.first() {
                    let end = *end;
                    for i in start..=end {
                        keys.push(vec![i]);
                    }
                }
                Ok(keys)
            }
            _ => Err(Error::InvalidOperation(
                "distinct keys are not available for unbounded ranges",
            )),
        }
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub fn keys_consume(self) -> Result<Vec<Vec<u8>>, Error> {
        match self {
            QueryItem::Key(key) => Ok(vec![key]),
            QueryItem::Range(Range { start, end }) => {
                let mut keys = vec![];
                if start.len() > 1 || end.len() != 1 {
                    return Err(Error::InvalidOperation(
                        "distinct keys are not available for ranges using more or less than 1 byte",
                    ));
                }
                let start = *start.first().unwrap_or_else(|| {
                    keys.push(vec![]);
                    &0
                });
                if let Some(end) = end.first() {
                    let end = *end;
                    for i in start..end {
                        keys.push(vec![i]);
                    }
                }
                Ok(keys)
            }
            QueryItem::RangeInclusive(range_inclusive) => {
                let start = range_inclusive.start();
                let end = range_inclusive.end();
                let mut keys = vec![];
                if start.len() > 1 || end.len() != 1 {
                    return Err(Error::InvalidOperation(
                        "distinct keys are not available for ranges using more or less than 1 byte",
                    ));
                }
                let start = *start.first().unwrap_or_else(|| {
                    keys.push(vec![]);
                    &0
                });
                if let Some(end) = end.first() {
                    let end = *end;
                    for i in start..=end {
                        keys.push(vec![i]);
                    }
                }
                Ok(keys)
            }
            _ => Err(Error::InvalidOperation(
                "distinct keys are not available for unbounded ranges",
            )),
        }
    }

    #[cfg(feature = "minimal")]
    pub fn seek_for_iter<I: RawIterator>(
        &self,
        iter: &mut I,
        left_to_right: bool,
    ) -> CostContext<()> {
        match self {
            QueryItem::Key(start) => iter.seek(start),
            QueryItem::Range(Range { start, end }) => {
                if left_to_right {
                    iter.seek(start)
                } else {
                    iter.seek(end).flat_map(|_| iter.prev())
                }
            }
            QueryItem::RangeInclusive(range_inclusive) => {
                if left_to_right {
                    iter.seek(range_inclusive.start())
                } else {
                    iter.seek_for_prev(range_inclusive.end())
                }
            }
            QueryItem::RangeFull(..) => {
                if left_to_right {
                    iter.seek_to_first()
                } else {
                    iter.seek_to_last()
                }
            }
            QueryItem::RangeFrom(RangeFrom { start }) => {
                if left_to_right {
                    iter.seek(start)
                } else {
                    iter.seek_to_last()
                }
            }
            QueryItem::RangeTo(RangeTo { end }) => {
                if left_to_right {
                    iter.seek_to_first()
                } else {
                    iter.seek(end).flat_map(|_| iter.prev())
                }
            }
            QueryItem::RangeToInclusive(RangeToInclusive { end }) => {
                if left_to_right {
                    iter.seek_to_first()
                } else {
                    iter.seek_for_prev(end)
                }
            }
            QueryItem::RangeAfter(RangeFrom { start }) => {
                if left_to_right {
                    let mut cost = OperationCost::default();
                    iter.seek(start).unwrap_add_cost(&mut cost);
                    if let Some(key) = iter.key().unwrap_add_cost(&mut cost) {
                        // if the key is the same as start we should go to next
                        if key == start {
                            iter.next().unwrap_add_cost(&mut cost)
                        }
                    }
                    ().wrap_with_cost(cost)
                } else {
                    iter.seek_to_last()
                }
            }
            QueryItem::RangeAfterTo(Range { start, end }) => {
                if left_to_right {
                    let mut cost = OperationCost::default();
                    iter.seek(start).unwrap_add_cost(&mut cost);
                    if let Some(key) = iter.key().unwrap_add_cost(&mut cost) {
                        // if the key is the same as start we тshould go to next
                        if key == start {
                            iter.next().unwrap_add_cost(&mut cost);
                        }
                    }
                    ().wrap_with_cost(cost)
                } else {
                    iter.seek(end).flat_map(|_| iter.prev())
                }
            }
            QueryItem::RangeAfterToInclusive(range_inclusive) => {
                if left_to_right {
                    let mut cost = OperationCost::default();
                    let start = range_inclusive.start();
                    iter.seek(start).unwrap_add_cost(&mut cost);
                    if let Some(key) = iter.key().unwrap_add_cost(&mut cost) {
                        // if the key is the same as start we тshould go to next
                        if key == start {
                            iter.next().unwrap_add_cost(&mut cost);
                        }
                    }
                    ().wrap_with_cost(cost)
                } else {
                    let end = range_inclusive.end();
                    iter.seek_for_prev(end)
                }
            }
        }
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub fn compare(a: &[u8], b: &[u8]) -> cmp::Ordering {
        for (ai, bi) in a.iter().zip(b.iter()) {
            match ai.cmp(bi) {
                Ordering::Equal => continue,
                ord => return ord,
            }
        }

        // if every single element was equal, compare length
        a.len().cmp(&b.len())
    }

    #[cfg(feature = "minimal")]
    pub fn iter_is_valid_for_type<I: RawIterator>(
        &self,
        iter: &I,
        limit: Option<u16>,
        left_to_right: bool,
    ) -> CostContext<bool> {
        let mut cost = OperationCost::default();

        // Check that if limit is set it's greater than 0 and iterator points to a valid
        // place.
        let basic_valid =
            limit.map(|l| l > 0).unwrap_or(true) && iter.valid().unwrap_add_cost(&mut cost);

        if !basic_valid {
            return false.wrap_with_cost(cost);
        }

        // Key should also be something, otherwise terminate early.
        let key = if let Some(key) = iter.key().unwrap_add_cost(&mut cost) {
            key
        } else {
            return false.wrap_with_cost(cost);
        };

        let is_valid = match self {
            QueryItem::Key(start) => key == start,
            QueryItem::Range(Range { start, end }) => {
                if left_to_right {
                    key < end
                } else {
                    key >= start
                }
            }
            QueryItem::RangeInclusive(range_inclusive) => {
                if left_to_right {
                    key <= range_inclusive.end()
                } else {
                    key >= range_inclusive.start()
                }
            }
            QueryItem::RangeFull(..) => {
                true // requires only basic validation which is done above
            }
            QueryItem::RangeFrom(RangeFrom { start }) => left_to_right || key >= start,
            QueryItem::RangeTo(RangeTo { end }) => !left_to_right || key < end,
            QueryItem::RangeToInclusive(RangeToInclusive { end }) => !left_to_right || key <= end,
            QueryItem::RangeAfter(RangeFrom { start }) => left_to_right || key > start,
            QueryItem::RangeAfterTo(Range { start, end }) => {
                if left_to_right {
                    key < end
                } else {
                    key > start
                }
            }
            QueryItem::RangeAfterToInclusive(range_inclusive) => {
                if left_to_right {
                    let end = range_inclusive.end().as_slice();
                    match Self::compare(key, end) {
                        Ordering::Less => true,
                        Ordering::Equal => true,
                        Ordering::Greater => false,
                    }
                } else {
                    let start = range_inclusive.start().as_slice();
                    match Self::compare(key, start) {
                        Ordering::Less => false,
                        Ordering::Equal => false,
                        Ordering::Greater => true,
                    }
                }
            }
        };

        is_valid.wrap_with_cost(cost)
    }

    #[cfg(any(feature = "minimal", feature = "verify"))]
    pub fn collides_with(&self, other: &Self) -> bool {
        self.intersect(other).in_both.is_some()
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl PartialEq<&[u8]> for QueryItem {
    fn eq(&self, other: &&[u8]) -> bool {
        matches!(self.partial_cmp(other), Some(Ordering::Equal))
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Ord for QueryItem {
    fn cmp(&self, other: &Self) -> Ordering {
        let self_as_range_set = self.to_range_set();
        let other_as_range_set = other.to_range_set();

        let compare_start = self_as_range_set.start.cmp(&other_as_range_set.start);

        // if start is equal then use the size of the set to compare
        // the smaller set is considered less
        if compare_start == Ordering::Equal {
            self_as_range_set.end.cmp(&other_as_range_set.end)
        } else {
            compare_start
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl PartialOrd for QueryItem {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl PartialOrd<&[u8]> for QueryItem {
    fn partial_cmp(&self, other: &&[u8]) -> Option<Ordering> {
        let other = Self::Key(other.to_vec());
        Some(self.cmp(&other))
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl From<Vec<u8>> for QueryItem {
    fn from(key: Vec<u8>) -> Self {
        Self::Key(key)
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod test {
    use crate::proofs::query::query_item::QueryItem;

    #[test]
    fn query_item_collides() {
        assert!(!QueryItem::Key(vec![10]).collides_with(&QueryItem::Key(vec![20])));
        assert!(QueryItem::Key(vec![10]).collides_with(&QueryItem::Key(vec![10])));
        assert!(!QueryItem::Key(vec![20]).collides_with(&QueryItem::Key(vec![10])));

        assert!(!QueryItem::Key(vec![10]).collides_with(&QueryItem::Range(vec![20]..vec![30])));
        assert!(QueryItem::Key(vec![10]).collides_with(&QueryItem::Range(vec![10]..vec![20])));
        assert!(QueryItem::Key(vec![15]).collides_with(&QueryItem::Range(vec![10]..vec![20])));
        assert!(!QueryItem::Key(vec![20]).collides_with(&QueryItem::Range(vec![10]..vec![20])));
        assert!(
            QueryItem::Key(vec![20]).collides_with(&QueryItem::RangeInclusive(vec![10]..=vec![20]))
        );
        assert!(!QueryItem::Key(vec![30]).collides_with(&QueryItem::Range(vec![10]..vec![20])));

        assert!(!QueryItem::Range(vec![10]..vec![20])
            .collides_with(&QueryItem::Range(vec![30]..vec![40])));
        assert!(!QueryItem::Range(vec![10]..vec![20])
            .collides_with(&QueryItem::Range(vec![20]..vec![30])));
        assert!(QueryItem::RangeInclusive(vec![10]..=vec![20])
            .collides_with(&QueryItem::Range(vec![20]..vec![30])));
        assert!(QueryItem::Range(vec![15]..vec![25])
            .collides_with(&QueryItem::Range(vec![20]..vec![30])));
        assert!(!QueryItem::Range(vec![20]..vec![30])
            .collides_with(&QueryItem::Range(vec![10]..vec![20])));
        assert!(QueryItem::RangeFrom(vec![2]..).collides_with(&QueryItem::Key(vec![5])));
    }

    #[test]
    fn query_item_cmp() {
        assert!(QueryItem::Key(vec![10]) < QueryItem::Key(vec![20]));
        assert_eq!(QueryItem::Key(vec![10]), QueryItem::Key(vec![10]));
        assert!(QueryItem::Key(vec![20]) > QueryItem::Key(vec![10]));

        assert!(QueryItem::Key(vec![10]) < QueryItem::Range(vec![20]..vec![30]));
        assert_ne!(
            QueryItem::Key(vec![10]),
            QueryItem::Range(vec![10]..vec![20])
        );
        assert_ne!(
            QueryItem::Key(vec![15]),
            QueryItem::Range(vec![10]..vec![20])
        );
        assert!(QueryItem::Key(vec![20]) > QueryItem::Range(vec![10]..vec![20]));
        assert_ne!(
            QueryItem::Key(vec![20]),
            QueryItem::RangeInclusive(vec![10]..=vec![20])
        );
        assert!(QueryItem::Key(vec![30]) > QueryItem::Range(vec![10]..vec![20]));

        assert!(QueryItem::Range(vec![10]..vec![20]) < QueryItem::Range(vec![30]..vec![40]));
        assert!(QueryItem::Range(vec![10]..vec![20]) < QueryItem::Range(vec![20]..vec![30]));
        assert_ne!(
            QueryItem::RangeInclusive(vec![10]..=vec![20]),
            QueryItem::Range(vec![20]..vec![30])
        );
        assert_ne!(
            QueryItem::Range(vec![15]..vec![25]),
            QueryItem::Range(vec![20]..vec![30])
        );
        assert!(QueryItem::Range(vec![20]..vec![30]) > QueryItem::Range(vec![10]..vec![20]));
    }
}
