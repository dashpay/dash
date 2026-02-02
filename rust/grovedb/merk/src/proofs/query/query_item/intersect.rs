use std::{
    cmp::Ordering,
    fmt,
    ops::{Range, RangeFrom, RangeFull, RangeInclusive, RangeTo, RangeToInclusive},
};

use crate::proofs::query::query_item::{
    intersect::RangeSetItem::{
        ExclusiveEnd, ExclusiveStart, Inclusive, UnboundedEnd, UnboundedStart,
    },
    QueryItem,
};

pub struct RangeSetIntersection {
    in_both: Option<RangeSet>,
    ours_left: Option<RangeSet>,
    ours_right: Option<RangeSet>,
    theirs_left: Option<RangeSet>,
    theirs_right: Option<RangeSet>,
}

/// Concise query item representation
#[derive(Clone, Debug)]
pub struct RangeSet {
    pub start: RangeSetItem,
    pub end: RangeSetItem,
}

/// Concise query item representation
#[derive(Clone, Copy, Debug)]
pub struct RangeSetBorrowed<'a> {
    pub start: RangeSetSimpleItemBorrowed<'a>,
    pub end: RangeSetSimpleItemBorrowed<'a>,
}

impl fmt::Display for RangeSetBorrowed<'_> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "[{} .. {}]", self.start, self.end)
    }
}

/// Specifies whether we are checking if there's at least one key
/// strictly less than (`LeftOf`) or strictly greater than (`RightOf`) a given
/// key.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    /// Checking if there's an item < `key`
    LeftOf,
    /// Checking if there's an item > `key`
    RightOf,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct KeyContainmentResult {
    pub included: bool,
    pub on_bounds_not_included: bool,
}

impl RangeSetBorrowed<'_> {
    /// Returns `true` if `key` is within [start, end] boundaries, respecting
    /// inclusive/exclusive semantics. If `start` is `Unbounded`, it is treated
    /// like -∞ (lowest possible). If `end` is `Unbounded`, it is treated like
    /// +∞ (highest possible).
    pub fn could_contain_key<K: AsRef<[u8]>>(&self, key: K) -> KeyContainmentResult {
        let key_bytes = key.as_ref();

        // 1) Check lower boundary (start)
        let (passes_start, on_start_exclusive) = match &self.start {
            RangeSetSimpleItemBorrowed::Unbounded => (true, false), // -∞ => all keys are >= -∞
            RangeSetSimpleItemBorrowed::Inclusive(bound_bytes) => {
                (key_bytes >= bound_bytes.as_slice(), false)
            }
            RangeSetSimpleItemBorrowed::Exclusive(bound_bytes) => (
                key_bytes > bound_bytes.as_slice(),
                key_bytes == bound_bytes.as_slice(),
            ),
        };

        // 2) Check upper boundary (end)
        let (passes_end, on_end_exclusive) = match &self.end {
            RangeSetSimpleItemBorrowed::Unbounded => (true, false), // +∞ => all keys are <= +∞
            RangeSetSimpleItemBorrowed::Inclusive(bound_bytes) => {
                (key_bytes <= bound_bytes.as_slice(), false)
            }
            RangeSetSimpleItemBorrowed::Exclusive(bound_bytes) => (
                key_bytes < bound_bytes.as_slice(),
                key_bytes == bound_bytes.as_slice(),
            ),
        };

        // 3) Key is contained only if it satisfies both constraints
        KeyContainmentResult {
            included: passes_start && passes_end,
            on_bounds_not_included: on_start_exclusive || on_end_exclusive,
        }
    }

    /// Checks if there's at least one item in [start, end] that is strictly
    /// to the *left* (< `key`) or *right* (> `key`) of `key`, depending on
    /// `direction`.
    ///
    /// - `Direction::LeftOf` => returns true if [start..end] might contain an
    ///   item < key.
    /// - `Direction::RightOf` => returns true if [start..end] might contain an
    ///   item > key.
    pub fn could_have_items_in_direction<K: AsRef<[u8]>>(
        &self,
        key: K,
        direction: Direction,
    ) -> bool {
        let key_bytes = key.as_ref();
        match direction {
            Direction::LeftOf => {
                // We want to know if there's any item in [start, end] that is < key.
                // That is possible if the start boundary is strictly less than `key`.
                match &self.start {
                    // Unbounded => effectively -∞ => definitely < key
                    RangeSetSimpleItemBorrowed::Unbounded => true,
                    // Inclusive(b) => items start at b. If b >= key, then no item is < key.
                    // Only if b < key, we might have something < key.
                    RangeSetSimpleItemBorrowed::Inclusive(b) => b.as_slice() < key_bytes,
                    // Exclusive(b) => items start strictly after b => if b < key,
                    // we still might have item < key. However if b == key, we start after key => no
                    // item < key.
                    RangeSetSimpleItemBorrowed::Exclusive(b) => b.as_slice() < key_bytes,
                }
            }
            Direction::RightOf => {
                // We want to know if there's any item in [start, end] that is > key.
                // That is possible if the end boundary is strictly greater than `key`.
                match &self.end {
                    // Unbounded => effectively +∞ => definitely > key
                    RangeSetSimpleItemBorrowed::Unbounded => true,
                    // Inclusive(b) => items extend up to b. If b <= key, then no item is > key.
                    // Only if b > key, we might have something > key.
                    RangeSetSimpleItemBorrowed::Inclusive(b) => b.as_slice() > key_bytes,
                    // Exclusive(b) => items extend up to but not including b.
                    // If b > key => there's space for an item > key. If b == key => no item > key.
                    RangeSetSimpleItemBorrowed::Exclusive(b) => b.as_slice() > key_bytes,
                }
            }
        }
    }
}

#[derive(Default)]
pub struct QueryItemManyIntersectionResult {
    pub in_both: Option<Vec<QueryItem>>,
    pub ours: Option<Vec<QueryItem>>,
    pub theirs: Option<Vec<QueryItem>>,
}

pub struct QueryItemIntersectionResultTheirsLeftovers {
    pub theirs_left: Option<QueryItem>,
    pub theirs_right: Option<QueryItem>,
}

impl QueryItemManyIntersectionResult {
    fn push_ours(&mut self, our_query_item: QueryItem) {
        let ours_vec = self.ours.get_or_insert(vec![]);
        ours_vec.push(our_query_item);
    }

    fn push_theirs(&mut self, their_query_item: QueryItem) {
        let theirs_vec = self.theirs.get_or_insert(vec![]);
        theirs_vec.push(their_query_item);
    }

    fn push_ours_and_in_both_from_result(
        &mut self,
        query_item_intersection_result: QueryItemIntersectionResult,
    ) -> QueryItemIntersectionResultTheirsLeftovers {
        let QueryItemIntersectionResult {
            in_both,
            ours_left,
            ours_right,
            theirs_left,
            theirs_right,
        } = query_item_intersection_result;
        if let Some(in_both) = in_both {
            let in_both_vec = self.in_both.get_or_insert(vec![]);
            in_both_vec.push(in_both);
        }
        if let Some(ours_left) = ours_left {
            let ours_vec = self.ours.get_or_insert(vec![]);
            ours_vec.push(ours_left);
        }
        if let Some(ours_right) = ours_right {
            let ours_vec = self.ours.get_or_insert(vec![]);
            ours_vec.push(ours_right);
        }

        QueryItemIntersectionResultTheirsLeftovers {
            theirs_left,
            theirs_right,
        }
    }

    #[allow(unused)]
    fn push_theirs_from_result(
        &mut self,
        query_item_intersection_result: QueryItemIntersectionResultTheirsLeftovers,
    ) {
        let QueryItemIntersectionResultTheirsLeftovers {
            theirs_left,
            theirs_right,
        } = query_item_intersection_result;
        if let Some(theirs_left) = theirs_left {
            let theirs_vec = self.theirs.get_or_insert(vec![]);
            theirs_vec.push(theirs_left);
        }
        if let Some(theirs_right) = theirs_right {
            let theirs_vec = self.theirs.get_or_insert(vec![]);
            theirs_vec.push(theirs_right);
        }
    }

    fn merge_in(&mut self, query_item_many_intersection_result: Self) {
        let QueryItemManyIntersectionResult {
            in_both,
            ours,
            theirs,
        } = query_item_many_intersection_result;
        if let Some(mut in_both) = in_both {
            let in_both_vec = self.in_both.get_or_insert(vec![]);
            in_both_vec.append(&mut in_both);
        }
        if let Some(mut ours) = ours {
            let ours_vec = self.ours.get_or_insert(vec![]);
            ours_vec.append(&mut ours);
        }
        if let Some(mut theirs) = theirs {
            let theirs_vec = self.theirs.get_or_insert(vec![]);
            theirs_vec.append(&mut theirs);
        }
    }
}

pub struct QueryItemIntersectionResult {
    pub in_both: Option<QueryItem>,
    pub ours_left: Option<QueryItem>,
    pub ours_right: Option<QueryItem>,
    pub theirs_left: Option<QueryItem>,
    pub theirs_right: Option<QueryItem>,
}

impl From<RangeSetIntersection> for QueryItemIntersectionResult {
    fn from(range_set_intersection: RangeSetIntersection) -> Self {
        Self {
            in_both: range_set_intersection.in_both.map(|a| a.to_query_item()),
            ours_left: range_set_intersection.ours_left.map(|a| a.to_query_item()),
            ours_right: range_set_intersection.ours_right.map(|a| a.to_query_item()),
            theirs_left: range_set_intersection
                .theirs_left
                .map(|a| a.to_query_item()),
            theirs_right: range_set_intersection
                .theirs_right
                .map(|a| a.to_query_item()),
        }
    }
}

impl RangeSet {
    // TODO: convert to impl of From/To trait
    pub fn to_query_item(&self) -> QueryItem {
        match (&self.start, &self.end) {
            (RangeSetItem::Inclusive(start), RangeSetItem::Inclusive(end)) => {
                if start == end {
                    QueryItem::Key(start.clone())
                } else {
                    QueryItem::RangeInclusive(RangeInclusive::new(start.clone(), end.clone()))
                }
            }
            (RangeSetItem::Inclusive(start), RangeSetItem::ExclusiveEnd(end)) => {
                QueryItem::Range(Range {
                    start: start.clone(),
                    end: end.clone(),
                })
            }
            (RangeSetItem::Inclusive(start), RangeSetItem::UnboundedEnd) => {
                QueryItem::RangeFrom(RangeFrom {
                    start: start.clone(),
                })
            }
            (RangeSetItem::ExclusiveStart(start), RangeSetItem::ExclusiveEnd(end)) => {
                QueryItem::RangeAfterTo(Range {
                    start: start.clone(),
                    end: end.clone(),
                })
            }
            (RangeSetItem::ExclusiveStart(start), RangeSetItem::Inclusive(end)) => {
                QueryItem::RangeAfterToInclusive(RangeInclusive::new(start.clone(), end.clone()))
            }
            (RangeSetItem::ExclusiveStart(start), RangeSetItem::UnboundedEnd) => {
                QueryItem::RangeAfter(RangeFrom {
                    start: start.clone(),
                })
            }
            (RangeSetItem::UnboundedStart, RangeSetItem::UnboundedEnd) => {
                QueryItem::RangeFull(RangeFull)
            }
            (RangeSetItem::UnboundedStart, RangeSetItem::Inclusive(end)) => {
                QueryItem::RangeToInclusive(RangeToInclusive { end: end.clone() })
            }
            (RangeSetItem::UnboundedStart, RangeSetItem::ExclusiveEnd(end)) => {
                QueryItem::RangeTo(RangeTo { end: end.clone() })
            }
            _ => {
                // TODO: return proper error, this should be unreachable
                //  if the range set was created from a valid query item,
                //  actually should return None in this case
                unreachable!()
            }
        }
    }

    pub fn intersect(&self, other: RangeSet) -> RangeSetIntersection {
        // check if the range sets do not overlap
        if self.end < other.start || other.end < self.start {
            // the sets do not overlap
            // no common element
            if self.end < other.start {
                // self is at the left
                return RangeSetIntersection {
                    in_both: None,
                    ours_left: Some(self.clone()),
                    ours_right: None,
                    theirs_right: Some(other),
                    theirs_left: None,
                };
            } else {
                return RangeSetIntersection {
                    in_both: None,
                    ours_left: None,
                    ours_right: Some(self.clone()),
                    theirs_left: Some(other),
                    theirs_right: None,
                };
            }
        }

        // sets overlap
        let (smaller_start, bigger_start) =
            RangeSetItem::order_items(&self.start, &other.start, self.start.cmp(&other.start));

        let (smaller_end, larger_end) =
            RangeSetItem::order_items(&self.end, &other.end, self.end.cmp(&other.end));

        // assume they are equal and progressively update the common boundary
        let mut intersection_result = RangeSetIntersection {
            in_both: Some(self.clone()),
            ours_left: None,
            ours_right: None,
            theirs_left: None,
            theirs_right: None,
        };

        // if the comparison of the start are not equal then we have value for left
        if self.start != other.start {
            if &self.start == smaller_start {
                // ours left
                intersection_result.ours_left = Some(RangeSet {
                    start: smaller_start.clone(),
                    end: bigger_start.invert(false),
                });
            } else {
                intersection_result.theirs_left = Some(RangeSet {
                    start: smaller_start.clone(),
                    end: bigger_start.invert(false),
                });
            }
        }

        // if the comparison of the end is not equal then we have value for right
        if self.end != other.end {
            if self.end > other.end {
                // ours right
                intersection_result.ours_right = Some(RangeSet {
                    start: smaller_end.invert(true),
                    end: larger_end.clone(),
                });
            } else {
                intersection_result.theirs_right = Some(RangeSet {
                    start: smaller_end.invert(true),
                    end: larger_end.clone(),
                });
            }
        }

        intersection_result.in_both = Some(RangeSet {
            start: bigger_start.clone(),
            end: smaller_end.clone(),
        });

        intersection_result
    }
}

/// Represents all possible value types in a range set
#[derive(Eq, PartialEq, Clone, Debug)]
pub enum RangeSetItem {
    UnboundedStart,
    UnboundedEnd,
    Inclusive(Vec<u8>),
    ExclusiveStart(Vec<u8>),
    ExclusiveEnd(Vec<u8>),
}

#[derive(Eq, PartialEq, Clone, Copy, Debug)]
pub enum RangeSetSimpleItemBorrowed<'a> {
    Unbounded,
    Inclusive(&'a Vec<u8>),
    Exclusive(&'a Vec<u8>),
}
impl fmt::Display for RangeSetSimpleItemBorrowed<'_> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            RangeSetSimpleItemBorrowed::Unbounded => write!(f, "Unbounded"),
            RangeSetSimpleItemBorrowed::Inclusive(b) => write!(f, "Inclusive({:X?})", b),
            RangeSetSimpleItemBorrowed::Exclusive(b) => write!(f, "Exclusive({:X?})", b),
        }
    }
}

impl RangeSetItem {
    pub fn invert(&self, is_start: bool) -> RangeSetItem {
        match &self {
            RangeSetItem::Inclusive(v) => {
                if is_start {
                    RangeSetItem::ExclusiveStart(v.clone())
                } else {
                    RangeSetItem::ExclusiveEnd(v.clone())
                }
            }
            RangeSetItem::ExclusiveStart(v) | RangeSetItem::ExclusiveEnd(v) => {
                RangeSetItem::Inclusive(v.clone())
            }
            RangeSetItem::UnboundedStart => RangeSetItem::UnboundedStart,
            RangeSetItem::UnboundedEnd => RangeSetItem::UnboundedEnd,
        }
    }

    /// Given som ordering and two items, this returns the items orders
    pub fn order_items<'a>(
        item_one: &'a RangeSetItem,
        item_two: &'a RangeSetItem,
        order: Ordering,
    ) -> (&'a RangeSetItem, &'a RangeSetItem) {
        match order {
            Ordering::Less => (item_one, item_two),
            _ => (item_two, item_one),
        }
    }
}

impl PartialOrd for RangeSetItem {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for RangeSetItem {
    fn cmp(&self, other: &Self) -> Ordering {
        match (self, other) {
            (UnboundedStart, UnboundedStart) => Ordering::Equal,
            (UnboundedEnd, UnboundedEnd) => Ordering::Equal,

            // unbounded start begins at negative infinity so it's smaller than all other values
            (UnboundedStart, _) => Ordering::Less,
            (_, UnboundedStart) => Ordering::Greater,

            // unbounded end stops at positive infinity so larger than all other values
            (UnboundedEnd, _) => Ordering::Greater,
            (_, UnboundedEnd) => Ordering::Less,

            (Inclusive(v1), Inclusive(v2))
            | (ExclusiveStart(v1), ExclusiveStart(v2))
            | (ExclusiveEnd(v1), ExclusiveEnd(v2)) => v1.cmp(v2),

            (Inclusive(v1), ExclusiveStart(v2)) | (ExclusiveEnd(v1), Inclusive(v2)) => {
                match v1.cmp(v2) {
                    Ordering::Equal | Ordering::Less => Ordering::Less,
                    _ => Ordering::Greater,
                }
            }
            (Inclusive(v1), ExclusiveEnd(v2)) | (ExclusiveStart(v1), Inclusive(v2)) => {
                match v1.cmp(v2) {
                    Ordering::Less => Ordering::Less,
                    _ => Ordering::Greater,
                }
            }

            (ExclusiveStart(v1), ExclusiveEnd(v2)) | (ExclusiveEnd(v2), ExclusiveStart(v1)) => {
                // start goes up, end goes down
                // if they are equal, exclusive end is smaller cause it stops just before the
                // number
                match v1.cmp(v2) {
                    Ordering::Equal | Ordering::Greater => Ordering::Greater,
                    _ => Ordering::Less,
                }
            }
        }
    }
}

impl QueryItem {
    pub fn intersect(&self, other: &Self) -> QueryItemIntersectionResult {
        self.to_range_set().intersect(other.to_range_set()).into()
    }

    // TODO: convert to impl of From/To trait
    pub fn to_range_set(&self) -> RangeSet {
        match self {
            QueryItem::Key(start) => RangeSet {
                start: RangeSetItem::Inclusive(start.clone()),
                end: RangeSetItem::Inclusive(start.clone()),
            },
            QueryItem::Range(range) => RangeSet {
                start: RangeSetItem::Inclusive(range.start.clone()),
                end: RangeSetItem::ExclusiveEnd(range.end.clone()),
            },
            QueryItem::RangeInclusive(range) => RangeSet {
                start: RangeSetItem::Inclusive(range.start().clone()),
                end: RangeSetItem::Inclusive(range.end().clone()),
            },
            QueryItem::RangeFull(..) => RangeSet {
                start: RangeSetItem::UnboundedStart,
                end: RangeSetItem::UnboundedEnd,
            },
            QueryItem::RangeFrom(range) => RangeSet {
                start: RangeSetItem::Inclusive(range.start.clone()),
                end: RangeSetItem::UnboundedEnd,
            },
            QueryItem::RangeTo(range) => RangeSet {
                start: RangeSetItem::UnboundedStart,
                end: RangeSetItem::ExclusiveEnd(range.end.clone()),
            },
            QueryItem::RangeToInclusive(range) => RangeSet {
                start: RangeSetItem::UnboundedStart,
                end: RangeSetItem::Inclusive(range.end.clone()),
            },
            QueryItem::RangeAfter(range) => RangeSet {
                start: RangeSetItem::ExclusiveStart(range.start.clone()),
                end: RangeSetItem::UnboundedEnd,
            },
            QueryItem::RangeAfterTo(range) => RangeSet {
                start: RangeSetItem::ExclusiveStart(range.start.clone()),
                end: RangeSetItem::ExclusiveEnd(range.end.clone()),
            },
            QueryItem::RangeAfterToInclusive(range) => RangeSet {
                start: RangeSetItem::ExclusiveStart(range.start().clone()),
                end: RangeSetItem::Inclusive(range.end().clone()),
            },
        }
    }

    // TODO: convert to impl of From/To trait
    pub fn to_range_set_borrowed(&self) -> Option<RangeSetBorrowed<'_>> {
        match self {
            QueryItem::Key(start) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Inclusive(start),
                end: RangeSetSimpleItemBorrowed::Inclusive(start),
            }),
            QueryItem::Range(range) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Inclusive(&range.start),
                end: RangeSetSimpleItemBorrowed::Exclusive(&range.end),
            }),
            QueryItem::RangeInclusive(range) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Inclusive(range.start()),
                end: RangeSetSimpleItemBorrowed::Inclusive(range.end()),
            }),
            QueryItem::RangeFull(..) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Unbounded,
                end: RangeSetSimpleItemBorrowed::Unbounded,
            }),
            QueryItem::RangeFrom(range) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Inclusive(&range.start),
                end: RangeSetSimpleItemBorrowed::Unbounded,
            }),
            QueryItem::RangeTo(range) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Unbounded,
                end: RangeSetSimpleItemBorrowed::Exclusive(&range.end),
            }),
            QueryItem::RangeToInclusive(range) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Unbounded,
                end: RangeSetSimpleItemBorrowed::Inclusive(&range.end),
            }),
            QueryItem::RangeAfter(range) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Exclusive(&range.start),
                end: RangeSetSimpleItemBorrowed::Unbounded,
            }),
            QueryItem::RangeAfterTo(range) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Exclusive(&range.start),
                end: RangeSetSimpleItemBorrowed::Exclusive(&range.end),
            }),
            QueryItem::RangeAfterToInclusive(range) => Some(RangeSetBorrowed {
                start: RangeSetSimpleItemBorrowed::Exclusive(range.start()),
                end: RangeSetSimpleItemBorrowed::Inclusive(range.end()),
            }),
        }
    }

    /// For this intersection to work ours and theirs must be ordered
    pub fn intersect_many_ordered(
        ours: &mut Vec<Self>,
        theirs: Vec<Self>,
    ) -> QueryItemManyIntersectionResult {
        let mut result = QueryItemManyIntersectionResult::default();
        for our_item in ours.drain(..) {
            // We create an intersection result for this one item
            let mut one_item_pair_intersections = QueryItemManyIntersectionResult::default();
            // We add our item
            // In the end the item might be split up
            one_item_pair_intersections.push_ours(our_item);
            for their_item in theirs.clone() {
                // We take the vector of our item
                // It might be empty if it has already been completely consumed
                // Meaning that all the item was inside of their items
                if let Some(our_item_split_sections) = one_item_pair_intersections.ours.take() {
                    let mut maybe_temp_their_item = Some(their_item);
                    for our_partial_item in our_item_split_sections {
                        if let Some(temp_their_item) = maybe_temp_their_item {
                            let intersection_result = our_partial_item.intersect(&temp_their_item);
                            // ours and in both are guaranteed to be unique
                            let theirs_leftovers = one_item_pair_intersections
                                .push_ours_and_in_both_from_result(intersection_result);
                            // if we assume theirs is ordered
                            // then we can push the left leftover
                            if let Some(theirs_left) = theirs_leftovers.theirs_left {
                                one_item_pair_intersections.push_theirs(theirs_left)
                            }
                            maybe_temp_their_item = theirs_leftovers.theirs_right
                        } else {
                            // there is no more of their item left
                            // just push our partial item
                            one_item_pair_intersections.push_ours(our_partial_item)
                        }
                    }
                    // we need to add the end theirs leftovers
                    if let Some(theirs_left) = maybe_temp_their_item {
                        one_item_pair_intersections.push_theirs(theirs_left)
                    }
                } else {
                    one_item_pair_intersections.push_theirs(their_item)
                }
            }
            result.merge_in(one_item_pair_intersections)
        }
        result
    }
}

#[cfg(test)]
mod test {
    use std::{
        cmp::Ordering,
        ops::{Range, RangeInclusive},
    };

    use crate::proofs::query::query_item::{intersect::RangeSetItem, QueryItem};

    #[test]
    pub fn test_range_set_query_item_conversion() {
        assert_eq!(
            QueryItem::Key(vec![5]).to_range_set().to_query_item(),
            QueryItem::Key(vec![5])
        );
        assert_eq!(
            QueryItem::Range(Range {
                start: vec![2],
                end: vec![5]
            })
            .to_range_set()
            .to_query_item(),
            QueryItem::Range(Range {
                start: vec![2],
                end: vec![5]
            })
        );
        assert_eq!(
            QueryItem::RangeInclusive(RangeInclusive::new(vec![2], vec![5]))
                .to_range_set()
                .to_query_item(),
            QueryItem::RangeInclusive(RangeInclusive::new(vec![2], vec![5]))
        );
        assert_eq!(
            QueryItem::RangeFull(..).to_range_set().to_query_item(),
            QueryItem::RangeFull(..)
        );
        assert_eq!(
            QueryItem::RangeFrom(vec![5]..)
                .to_range_set()
                .to_query_item(),
            QueryItem::RangeFrom(vec![5]..)
        );
        assert_eq!(
            QueryItem::RangeTo(..vec![3]).to_range_set().to_query_item(),
            QueryItem::RangeTo(..vec![3])
        );
        assert_eq!(
            QueryItem::RangeToInclusive(..=vec![3])
                .to_range_set()
                .to_query_item(),
            QueryItem::RangeToInclusive(..=vec![3])
        );
        assert_eq!(
            QueryItem::RangeAfter(vec![4]..)
                .to_range_set()
                .to_query_item(),
            QueryItem::RangeAfter(vec![4]..)
        );
        assert_eq!(
            QueryItem::RangeAfterTo(vec![3]..vec![6])
                .to_range_set()
                .to_query_item(),
            QueryItem::RangeAfterTo(vec![3]..vec![6])
        );
        assert_eq!(
            QueryItem::RangeAfterToInclusive(vec![3]..=vec![7])
                .to_range_set()
                .to_query_item(),
            QueryItem::RangeAfterToInclusive(vec![3]..=vec![7])
        );
    }

    #[test]
    pub fn test_range_set_item_compare() {
        // doing a pyramid compare, to prevent repeated test
        // if we compare A and B, we don't compare B and A further down

        // test equality
        assert_eq!(
            RangeSetItem::Inclusive(vec![1]).cmp(&RangeSetItem::Inclusive(vec![1])),
            Ordering::Equal
        );
        assert_eq!(
            RangeSetItem::ExclusiveStart(vec![1]).cmp(&RangeSetItem::ExclusiveStart(vec![1])),
            Ordering::Equal
        );
        assert_eq!(
            RangeSetItem::ExclusiveEnd(vec![1]).cmp(&RangeSetItem::ExclusiveEnd(vec![1])),
            Ordering::Equal
        );
        assert_eq!(
            RangeSetItem::UnboundedStart.cmp(&RangeSetItem::UnboundedStart),
            Ordering::Equal
        );
        assert_eq!(
            RangeSetItem::UnboundedEnd.cmp(&RangeSetItem::UnboundedEnd),
            Ordering::Equal
        );

        // test same item but less value
        assert_eq!(
            RangeSetItem::Inclusive(vec![1]).cmp(&RangeSetItem::Inclusive(vec![2])),
            Ordering::Less
        );
        assert_eq!(
            RangeSetItem::ExclusiveStart(vec![1]).cmp(&RangeSetItem::ExclusiveStart(vec![2])),
            Ordering::Less
        );
        assert_eq!(
            RangeSetItem::ExclusiveEnd(vec![1]).cmp(&RangeSetItem::ExclusiveEnd(vec![2])),
            Ordering::Less
        );

        // test same item but greater value
        assert_eq!(
            RangeSetItem::Inclusive(vec![3]).cmp(&RangeSetItem::Inclusive(vec![2])),
            Ordering::Greater
        );
        assert_eq!(
            RangeSetItem::ExclusiveStart(vec![3]).cmp(&RangeSetItem::ExclusiveStart(vec![2])),
            Ordering::Greater
        );
        assert_eq!(
            RangeSetItem::ExclusiveEnd(vec![3]).cmp(&RangeSetItem::ExclusiveEnd(vec![2])),
            Ordering::Greater
        );

        // unbounded end is greater than everything
        // tried creating the maximum possible vector with vec![u8::MAX; isize::MAX as
        // usize])) but got memory allocation problems
        assert_eq!(
            RangeSetItem::UnboundedEnd.cmp(&RangeSetItem::Inclusive(vec![u8::MAX; 1000])),
            Ordering::Greater
        );
        assert_eq!(
            RangeSetItem::UnboundedEnd.cmp(&RangeSetItem::ExclusiveStart(vec![u8::MAX; 1000])),
            Ordering::Greater
        );
        assert_eq!(
            RangeSetItem::UnboundedEnd.cmp(&RangeSetItem::ExclusiveEnd(vec![u8::MAX; 1000])),
            Ordering::Greater
        );
        assert_eq!(
            RangeSetItem::UnboundedEnd.cmp(&RangeSetItem::UnboundedStart),
            Ordering::Greater
        );

        // unbounded start is less than everything
        assert_eq!(
            RangeSetItem::UnboundedStart.cmp(&RangeSetItem::Inclusive(vec![])),
            Ordering::Less
        );
        assert_eq!(
            RangeSetItem::UnboundedStart.cmp(&RangeSetItem::ExclusiveStart(vec![])),
            Ordering::Less
        );
        assert_eq!(
            RangeSetItem::UnboundedStart.cmp(&RangeSetItem::ExclusiveEnd(vec![])),
            Ordering::Less
        );

        // test inclusive
        // exclusive start represents value + step_size
        // if step size is 1 and value is 1 then it starts at 2 (basically excluding 1)
        // hence inclusive at 1 is less since 1 < 2
        assert_eq!(
            RangeSetItem::Inclusive(vec![1]).cmp(&RangeSetItem::ExclusiveStart(vec![1])),
            Ordering::Less
        );
        assert_eq!(
            RangeSetItem::Inclusive(vec![0]).cmp(&RangeSetItem::ExclusiveStart(vec![1])),
            Ordering::Less
        );
        assert_eq!(
            RangeSetItem::Inclusive(vec![2]).cmp(&RangeSetItem::ExclusiveStart(vec![1])),
            Ordering::Greater
        );
        // exclusive end represents value - step_size
        // if step size is 1 and value is 1 then it represents at 0 (includes everything
        // before 1) hence inclusive at 1 is greater since 1 > 0
        assert_eq!(
            RangeSetItem::Inclusive(vec![1]).cmp(&RangeSetItem::ExclusiveEnd(vec![1])),
            Ordering::Greater
        );
        assert_eq!(
            RangeSetItem::Inclusive(vec![0]).cmp(&RangeSetItem::ExclusiveEnd(vec![1])),
            Ordering::Less
        );
        assert_eq!(
            RangeSetItem::Inclusive(vec![2]).cmp(&RangeSetItem::ExclusiveEnd(vec![1])),
            Ordering::Greater
        );

        // test exclusive start
        // exclusive start is greater than exclusive end for >= same value
        assert_eq!(
            RangeSetItem::ExclusiveStart(vec![1]).cmp(&RangeSetItem::ExclusiveEnd(vec![1])),
            Ordering::Greater
        );
        assert_eq!(
            RangeSetItem::ExclusiveStart(vec![2]).cmp(&RangeSetItem::ExclusiveEnd(vec![1])),
            Ordering::Greater
        );
        // but less when the value is less
        assert_eq!(
            RangeSetItem::ExclusiveStart(vec![1]).cmp(&RangeSetItem::ExclusiveEnd(vec![2])),
            Ordering::Less
        );
    }
}
