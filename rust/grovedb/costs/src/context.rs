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

use crate::OperationCost;

/// Wrapped operation result with associated cost.
#[must_use]
#[derive(Debug, Eq, PartialEq)]
pub struct CostContext<T> {
    /// Wrapped operation's return value.
    pub value: T,
    /// Cost of the operation.
    pub cost: OperationCost,
}

/// General combinators for `CostContext`.
impl<T> CostContext<T> {
    /// Take wrapped value out adding its cost to provided accumulator.
    pub fn unwrap_add_cost(self, acc_cost: &mut OperationCost) -> T {
        *acc_cost += self.cost;
        self.value
    }

    /// Take wrapped value out dropping cost data.
    pub fn unwrap(self) -> T {
        self.value
    }

    /// Borrow costs data.
    pub fn cost(&self) -> &OperationCost {
        &self.cost
    }

    /// Borrow wrapped data.
    pub fn value(&self) -> &T {
        &self.value
    }

    /// Applies function to wrapped value keeping cost the same as before.
    pub fn map<B>(self, f: impl FnOnce(T) -> B) -> CostContext<B> {
        let cost = self.cost;
        let value = f(self.value);
        CostContext { value, cost }
    }

    /// Applies function to wrapped value adding costs.
    pub fn flat_map<B>(self, f: impl FnOnce(T) -> CostContext<B>) -> CostContext<B> {
        let mut cost = self.cost;
        let value = f(self.value).unwrap_add_cost(&mut cost);
        CostContext { value, cost }
    }

    /// Adds previously accumulated cost
    pub fn add_cost(mut self, cost: OperationCost) -> Self {
        self.cost += cost;
        self
    }
}

/// Type alias for `Result` wrapped into `CostContext`.
pub type CostResult<T, E> = CostContext<Result<T, E>>;

/// Combinators to use with `Result` wrapped in `CostContext`.
impl<T, E> CostResult<T, E> {
    /// Applies function to wrapped value in case of `Ok` keeping cost the same
    /// as before.
    pub fn map_ok<B>(self, f: impl FnOnce(T) -> B) -> CostResult<B, E> {
        self.map(|result| result.map(f))
    }

    /// Applies function to wrapped value in case of `Err` keeping cost the same
    /// as before.
    pub fn map_err<B>(self, f: impl FnOnce(E) -> B) -> CostResult<T, B> {
        self.map(|result| result.map_err(f))
    }

    /// Applies function to wrapped result in case of `Ok` adding costs.
    pub fn flat_map_ok<B>(self, f: impl FnOnce(T) -> CostResult<B, E>) -> CostResult<B, E> {
        let mut cost = self.cost;
        let result = match self.value {
            Ok(x) => f(x).unwrap_add_cost(&mut cost),
            Err(e) => Err(e),
        };
        CostContext {
            value: result,
            cost,
        }
    }

    /// Gets the cost as a result
    pub fn cost_as_result(self) -> Result<OperationCost, E> {
        self.value.map(|_| self.cost)
    }

    /// Call the provided function on success without altering result or cost.
    pub fn for_ok(self, f: impl FnOnce(&T)) -> CostResult<T, E> {
        if let Ok(x) = &self.value {
            f(x)
        }

        self
    }
}

impl<T, E> CostResult<Result<T, E>, E> {
    /// Flattens nested errors inside `CostContext`
    pub fn flatten(self) -> CostResult<T, E> {
        self.map(|value| match value {
            Err(e) => Err(e),
            Ok(Err(e)) => Err(e),
            Ok(Ok(v)) => Ok(v),
        })
    }
}

impl<T> CostContext<CostContext<T>> {
    /// Flattens nested `CostContext`s adding costs.
    pub fn flatten(self) -> CostContext<T> {
        let mut cost = OperationCost::default();
        let inner = self.unwrap_add_cost(&mut cost);
        inner.add_cost(cost)
    }
}

/// Extension trait to add costs context to values.
pub trait CostsExt {
    /// Wraps any value into a `CostContext` object with provided costs.
    fn wrap_with_cost(self, cost: OperationCost) -> CostContext<Self>
    where
        Self: Sized,
    {
        CostContext { value: self, cost }
    }

    /// Wraps any value into `CostContext` object with costs computed using the
    /// value getting wrapped.
    fn wrap_fn_cost(self, f: impl FnOnce(&Self) -> OperationCost) -> CostContext<Self>
    where
        Self: Sized,
    {
        CostContext {
            cost: f(&self),
            value: self,
        }
    }
}

impl<T> CostsExt for T {}

/// Macro to achieve kind of what the `?` operator does, but with `CostContext`
/// on top.
///
/// Main properties are:
/// 1. Early termination on error;
/// 2. Because of 1, `Result` is removed from the equation;
/// 3. `CostContext` is removed too because it is added to external cost
///    accumulator;
/// 4. Early termination uses external cost accumulator so previous costs won't
///    be lost.
#[macro_export]
macro_rules! cost_return_on_error {
    ( &mut $cost:ident, $($body:tt)+ ) => {
        {
            use $crate::CostsExt;
            let result_with_cost = { $($body)+ };
            let result = result_with_cost.unwrap_add_cost(&mut $cost);
            match result {
                Ok(x) => x,
                Err(e) => return Err(e).wrap_with_cost($cost),
            }
        }
    };
}

/// Macro to achieve kind of what the `?` operator does, but with `CostContext`
/// on top.
///
/// Main properties are:
/// 1. Early termination on error;
/// 2. Because of 1, `Result` is removed from the equation;
/// 3. `CostContext` is removed too because it is added to external cost
///    accumulator;
/// 4. Early termination uses external cost accumulator so previous costs won't
///    be lost.
#[macro_export]
macro_rules! cost_return_on_error_into {
    ( &mut $cost:ident, $($body:tt)+ ) => {
        {
            use $crate::CostsExt;
            let result_with_cost = { $($body)+ };
            let result = result_with_cost.unwrap_add_cost(&mut $cost);
            match result {
                Ok(x) => x,
                Err(e) => return Err(e.into()).wrap_with_cost($cost),
            }
        }
    };
}

/// Macro to achieve kind of what the `?` operator does, but with `CostContext`
/// on top. The difference between this macro and `cost_return_on_error` is that
/// it is intended to be used on `Result` rather than `CostContext<Result<..>>`,
/// so no costs will be added except previously accumulated.
#[macro_export]
macro_rules! cost_return_on_error_no_add {
    ( $cost:ident, $($body:tt)+ ) => {
        {
            use $crate::CostsExt;
            let result = { $($body)+ };
            match result {
                Ok(x) => x,
                Err(e) => return Err(e).wrap_with_cost($cost),
            }
        }
    };
}

/// Macro to achieve kind of what the `?` operator does, but with `CostContext`
/// on top. The difference between this macro and `cost_return_on_error` is that
/// it is intended to be used on `Result` rather than `CostContext<Result<..>>`,
/// so no costs will be added except previously accumulated.
#[macro_export]
macro_rules! cost_return_on_error_into_no_add {
    ( $cost:ident, $($body:tt)+ ) => {
        {
            use $crate::CostsExt;
            let result = { $($body)+ };
            match result {
                Ok(x) => x,
                Err(e) => return Err(e.into()).wrap_with_cost($cost),
            }
        }
    };
}

/// Macro to achieve kind of what the `?` operator does, but with `CostContext`
/// on top. The difference between this macro and `cost_return_on_error` is that
/// it is intended to be used on `Result` rather than `CostContext<Result<..>>`,
/// so no costs will be added except previously accumulated. The error case
/// returns a default `OperationCost`.
#[macro_export]
macro_rules! cost_return_on_error_default {
    ( $($body:tt)+ ) => {
        {
            use $crate::CostsExt;
            let result = { $($body)+ };
            match result {
                Ok(x) => x,
                Err(e) => return Err(e).wrap_with_cost(OperationCost::default()),
            }
        }
    };
}

/// Macro to achieve kind of what the `?` operator does, but with `CostContext`
/// on top. The difference between this macro and `cost_return_on_error` is that
/// it is intended to be used on `Result` rather than `CostContext<Result<..>>`,
/// so no costs will be added except previously accumulated. The error case
/// returns a default `OperationCost`.
#[macro_export]
macro_rules! cost_return_on_error_into_default {
    ( $($body:tt)+ ) => {
        {
            use $crate::CostsExt;
            let result = { $($body)+ };
            match result {
                Ok(x) => x,
                Err(e) => return Err(e.into()).wrap_with_cost(OperationCost::default()),
            }
        }
    };
}
