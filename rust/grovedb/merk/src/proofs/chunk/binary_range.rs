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

const LEFT: bool = true;
const RIGHT: bool = false;

/// Utility type for range bisection and advancement
#[derive(Debug)]
pub(crate) struct BinaryRange {
    start: usize,
    end: usize,
}

impl BinaryRange {
    /// Returns a new BinaryRange and ensures that start < end
    /// and min start value is 1
    pub fn new(start: usize, end: usize) -> Result<Self, String> {
        // start should be less than or equal to end
        if start > end {
            return Err(String::from("start value cannot be greater than end value"));
        }

        // the minimum value for start should be 1
        // that way the length of the maximum length
        // of the range is usize::MAX and not
        // usize::MAX + 1
        if start < 1 {
            return Err(String::from(
                "minimum start value should be 1 to avoid len overflow",
            ));
        }

        Ok(Self { start, end })
    }

    /// Returns the len of the current range
    pub fn len(&self) -> usize {
        self.end - self.start + 1
    }

    /// Returns true when the len of the range is odd
    pub fn odd(&self) -> bool {
        (self.len() % 2) != 0
    }

    /// Determines if a value belongs to the left half or right half of a range
    /// returns true for left and false for right
    /// returns None if value is outside the range or range len is odd
    pub fn which_half(&self, value: usize) -> Option<bool> {
        // return None if value is not in the range
        if value < self.start || value > self.end {
            return None;
        }

        // can't divide the range into equal halves
        // when odd, so return None
        if self.odd() {
            return None;
        }

        let half_size = self.len() / 2;
        let second_half_start = self.start + half_size;

        if value >= second_half_start {
            return Some(RIGHT);
        }

        Some(LEFT)
    }

    /// Returns a new range that only contains elements on the specified half
    /// returns an error if range is not odd
    pub fn get_half(&self, left: bool) -> Result<Self, String> {
        if self.odd() {
            return Err(String::from("cannot break odd range in half"));
        }

        let half_size = self.len() / 2;
        let second_half_start = self.start + half_size;

        Ok(if left {
            Self {
                start: self.start,
                end: second_half_start - 1,
            }
        } else {
            Self {
                start: second_half_start,
                end: self.end,
            }
        })
    }

    /// Returns a new range that increments the start value
    /// also return the previous start value
    /// returns an error if the operation will cause start to be larger than end
    pub fn advance_range_start(&self) -> Result<(Self, usize), String> {
        // check if operation will cause start > end
        if self.start == self.end {
            return Err(String::from(
                "can't advance start when start is equal to end",
            ));
        }

        Ok((
            Self {
                start: self.start + 1,
                end: self.end,
            },
            self.start,
        ))
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn cannot_create_invalid_range() {
        let invalid_range = BinaryRange::new(5, 3);
        assert!(invalid_range.is_err());
    }

    #[test]
    fn can_get_range_len() {
        let range = BinaryRange::new(2, 5).expect("should create range");
        assert_eq!(range.len(), 4);
        assert!(!range.odd());

        let range = BinaryRange::new(2, 2).expect("should create range");
        assert_eq!(range.len(), 1);
        assert!(range.odd());
    }

    #[test]
    fn can_determine_correct_half() {
        let range = BinaryRange::new(3, 7).expect("should create range");
        assert_eq!(range.len(), 5);
        assert!(range.odd());

        // cannot determine half for value outside a range
        assert!(range.which_half(1).is_none());
        assert!(range.which_half(7).is_none());

        // cannot determine half when range is odd
        assert!(range.which_half(3).is_none());

        let range = BinaryRange::new(3, 6).expect("should create range");
        assert_eq!(range.len(), 4);
        assert!(!range.odd());

        assert_eq!(range.which_half(3), Some(LEFT));
        assert_eq!(range.which_half(4), Some(LEFT));
        assert_eq!(range.which_half(5), Some(RIGHT));
        assert_eq!(range.which_half(6), Some(RIGHT));
    }

    #[test]
    fn can_advance_start_range() {
        let range = BinaryRange::new(2, 5).expect("should create range");
        assert_eq!(range.len(), 4);
        assert_eq!(range.start, 2);

        // advance the range
        let (range, prev_start) = range.advance_range_start().expect("should advance range");
        assert_eq!(prev_start, 2);
        assert_eq!(range.len(), 3);
        assert_eq!(range.start, 3);

        // advance range
        let (range, prev_start) = range.advance_range_start().expect("should advance range");
        assert_eq!(prev_start, 3);
        assert_eq!(range.len(), 2);
        assert_eq!(range.start, 4);

        // advance range
        let (range, prev_start) = range.advance_range_start().expect("should advance range");
        assert_eq!(prev_start, 4);
        assert_eq!(range.len(), 1);
        assert_eq!(range.start, 5);

        // should not be allowed to advance the range anymore
        let advance_result = range.advance_range_start();
        assert!(advance_result.is_err());
    }

    #[test]
    fn can_break_range_into_halves() {
        let range = BinaryRange::new(2, 10).expect("should create range");
        assert_eq!(range.len(), 9);
        assert!(range.odd());
        assert!(range.get_half(LEFT).is_err());

        let range = BinaryRange::new(2, 11).expect("should create range");
        assert_eq!(range.len(), 10);
        assert!(!range.odd());

        let left_range = range.get_half(LEFT).expect("should get sub range");
        assert_eq!(left_range.start, 2);
        assert_eq!(left_range.end, 6);

        let right_range = range.get_half(RIGHT).expect("should get sub range");
        assert_eq!(right_range.start, 7);
        assert_eq!(right_range.end, 11);

        // right_range is false, advance to make even
        let (right_range, _prev) = right_range.advance_range_start().expect("should advance");
        let right_left_range = right_range.get_half(LEFT).expect("should get sub range");
        assert_eq!(right_left_range.len(), 2);
        assert_eq!(right_left_range.start, 8);
        assert_eq!(right_left_range.end, 9);
    }
}
