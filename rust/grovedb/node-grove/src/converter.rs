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

//! Converter

use grovedb::{reference_path::ReferencePathType, Element, PathQuery, Query, SizedQuery};
use neon::{prelude::*, types::buffer::TypedArray};

fn element_to_string(element: Element) -> String {
    match element {
        Element::Item(..) => "item".to_string(),
        Element::SumItem(..) => "sum_item".to_string(),
        Element::ItemWithSumItem(..) => "item_with_sum_item".to_string(),
        Element::Reference(..) => "reference".to_string(),
        Element::Tree(..) => "tree".to_string(),
        Element::SumTree(..) => "sum_tree".to_string(),
        Element::BigSumTree(..) => "big_sum_tree".to_string(),
        Element::CountTree(..) => "count_tree".to_string(),
        Element::CountSumTree(..) => "count_sum_tree".to_string(),
        Element::ProvableCountTree(..) => "provable_count_tree".to_string(),
        Element::ProvableCountSumTree(..) => "provable_count_sum_tree".to_string(),
    }
}

/// Convert js object to element
pub fn js_object_to_element<'a, C: Context<'a>>(
    js_object: Handle<JsObject>,
    cx: &mut C,
) -> NeonResult<Element> {
    let js_element_string: Handle<JsString> = js_object.get(cx, "type")?;

    let element_string: String = js_element_string.value(cx);

    match element_string.as_str() {
        "item" => {
            let js_buffer: Handle<JsBuffer> = js_object.get(cx, "value")?;
            let item = js_buffer_to_vec_u8(js_buffer, cx);
            Ok(Element::new_item(item))
        }
        "reference" => {
            let js_array: Handle<JsArray> = js_object.get(cx, "value")?;
            let reference = js_array_of_buffers_to_vec(js_array, cx)?;
            // TODO: Fix bindings
            Ok(Element::new_reference(
                ReferencePathType::AbsolutePathReference(reference),
            ))
        }
        "tree" => {
            let js_buffer: Handle<JsBuffer> = js_object.get(cx, "value")?;
            let tree_vec = js_buffer_to_vec_u8(js_buffer, cx);
            Ok(Element::new_tree(Some(tree_vec)))
        }
        "provable_count_tree" => {
            let js_buffer: Handle<JsBuffer> = js_object.get(cx, "value")?;
            let tree_vec = js_buffer_to_vec_u8(js_buffer, cx);
            Ok(Element::new_provable_count_tree(Some(tree_vec)))
        }
        _ => cx.throw_error(format!("Unexpected element type {element_string}")),
    }
}

/// Convert element to js object
pub fn element_to_js_object<'a, C: Context<'a>>(
    element: Element,
    cx: &mut C,
) -> NeonResult<Handle<'a, JsValue>> {
    let js_object = cx.empty_object();
    let js_type_string = cx.string(element_to_string(element.clone()));
    js_object.set(cx, "type", js_type_string)?;

    let js_value: Handle<JsValue> = match element {
        Element::Item(item, _) => {
            let js_buffer = JsBuffer::external(cx, item);
            js_buffer.upcast()
        }
        // TODO: Fix bindings
        Element::SumItem(..) => nested_vecs_to_js(vec![], cx)?,
        Element::ItemWithSumItem(item, ..) => {
            let js_buffer = JsBuffer::external(cx, item);
            js_buffer.upcast()
        }
        Element::Reference(..) => nested_vecs_to_js(vec![], cx)?,
        Element::Tree(..) => nested_vecs_to_js(vec![], cx)?,
        Element::SumTree(..) => nested_vecs_to_js(vec![], cx)?,
        Element::BigSumTree(..) => nested_vecs_to_js(vec![], cx)?,
        Element::CountTree(..) => nested_vecs_to_js(vec![], cx)?,
        Element::CountSumTree(..) => nested_vecs_to_js(vec![], cx)?,
        Element::ProvableCountTree(..) => nested_vecs_to_js(vec![], cx)?,
        Element::ProvableCountSumTree(..) => nested_vecs_to_js(vec![], cx)?,
    };

    js_object.set(cx, "value", js_value)?;
    NeonResult::Ok(js_object.upcast())
}

/// Convert nested vecs to js
pub fn nested_vecs_to_js<'a, C: Context<'a>>(
    v: Vec<Vec<u8>>,
    cx: &mut C,
) -> NeonResult<Handle<'a, JsValue>> {
    let js_array: Handle<JsArray> = cx.empty_array();

    for (index, bytes) in v.iter().enumerate() {
        let js_buffer = JsBuffer::external(cx, bytes.clone());
        let js_value = js_buffer.as_value(cx);
        js_array.set(cx, index as u32, js_value)?;
    }

    Ok(js_array.upcast())
}

/// Convert js buffer to vec
pub fn js_buffer_to_vec_u8<'a, C: Context<'a>>(js_buffer: Handle<JsBuffer>, cx: &mut C) -> Vec<u8> {
    js_buffer.as_slice(cx).to_vec()
}

/// Convert js array of buffers to vec
pub fn js_array_of_buffers_to_vec<'a, C: Context<'a>>(
    js_array: Handle<JsArray>,
    cx: &mut C,
) -> NeonResult<Vec<Vec<u8>>> {
    let buf_vec = js_array.to_vec(cx)?;
    let mut vec: Vec<Vec<u8>> = Vec::new();

    for buf in buf_vec {
        let js_buffer_handle = buf.downcast_or_throw::<JsBuffer, _>(cx)?;
        vec.push(js_buffer_to_vec_u8(js_buffer_handle, cx));
    }

    Ok(vec)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn element_to_string_identifies_item_with_sum_item() {
        let element = Element::ItemWithSumItem(b"node".to_vec(), 4, Some(vec![1]));
        assert_eq!(element_to_string(element), "item_with_sum_item");
    }
}

/// Convert js value to option
pub fn js_value_to_option<'a, T: Value, C: Context<'a>>(
    js_value: Handle<'a, JsValue>,
    cx: &mut C,
) -> NeonResult<Option<Handle<'a, T>>> {
    if js_value.is_a::<JsNull, _>(cx) || js_value.is_a::<JsUndefined, _>(cx) {
        Ok(None)
    } else {
        Ok(Some(js_value.downcast_or_throw::<T, _>(cx)?))
    }
}

fn js_object_get_vec_u8<'a, C: Context<'a>>(
    js_object: Handle<JsObject>,
    field: &str,
    cx: &mut C,
) -> NeonResult<Vec<u8>> {
    Ok(js_buffer_to_vec_u8(js_object.get(cx, field)?, cx))
}

/// Convert js object to query
fn js_object_to_query<'a, C: Context<'a>>(
    js_object: Handle<JsObject>,
    cx: &mut C,
) -> NeonResult<Query> {
    let items: Handle<JsArray> = js_object.get(cx, "items")?;
    let mut query = Query::new();
    for js_item in items.to_vec(cx)? {
        let item = js_item.downcast_or_throw::<JsObject, _>(cx)?;
        let item_str: Handle<JsString> = item.get(cx, "type")?;
        match item_str.value(cx).as_ref() {
            "key" => {
                query.insert_key(js_object_get_vec_u8(item, "key", cx)?);
            }
            "range" => {
                let from = js_object_get_vec_u8(item, "from", cx)?;
                let to = js_object_get_vec_u8(item, "to", cx)?;
                query.insert_range(from..to);
            }
            "rangeInclusive" => {
                let from = js_object_get_vec_u8(item, "from", cx)?;
                let to = js_object_get_vec_u8(item, "to", cx)?;
                query.insert_range_inclusive(from..=to);
            }
            "rangeFull" => {
                query.insert_all();
            }
            "rangeFrom" => {
                query.insert_range_from(js_object_get_vec_u8(item, "from", cx)?..);
            }
            "rangeTo" => {
                query.insert_range_to(..js_object_get_vec_u8(item, "to", cx)?);
            }
            "rangeToInclusive" => {
                query.insert_range_to_inclusive(..=js_object_get_vec_u8(item, "to", cx)?);
            }
            "rangeAfter" => {
                query.insert_range_after(js_object_get_vec_u8(item, "after", cx)?..);
            }
            "rangeAfterTo" => {
                let after = js_object_get_vec_u8(item, "after", cx)?;
                let to = js_object_get_vec_u8(item, "to", cx)?;
                query.insert_range_after_to(after..to);
            }
            "rangeAfterToInclusive" => {
                let after = js_object_get_vec_u8(item, "after", cx)?;
                let to = js_object_get_vec_u8(item, "to", cx)?;
                query.insert_range_after_to_inclusive(after..=to);
            }
            _ => {
                cx.throw_range_error("query item type is not supported")?;
            }
        }
    }

    let subquery_path = js_value_to_option::<JsArray, _>(js_object.get(cx, "subqueryPath")?, cx)?
        .map(|x| js_array_of_buffers_to_vec(x, cx))
        .transpose();
    let subquery = js_value_to_option::<JsObject, _>(js_object.get(cx, "subquery")?, cx)?
        .map(|x| js_object_to_query(x, cx))
        .transpose()?;
    let left_to_right = js_value_to_option::<JsBoolean, _>(js_object.get(cx, "leftToRight")?, cx)?
        .map(|x| x.value(cx));

    query.default_subquery_branch.subquery_path = subquery_path.unwrap();
    query.default_subquery_branch.subquery = subquery.map(Box::new);
    query.left_to_right = left_to_right.unwrap_or(true);

    Ok(query)
}

/// Convert js object to sized query
fn js_object_to_sized_query<'a, C: Context<'a>>(
    js_object: Handle<JsObject>,
    cx: &mut C,
) -> NeonResult<SizedQuery> {
    let query = js_object_to_query(js_object.get(cx, "query")?, cx)?;
    let limit: Option<u16> = js_value_to_option::<JsNumber, _>(js_object.get(cx, "limit")?, cx)?
        .map(|x| {
            u16::try_from(x.value(cx) as i64)
                .or_else(|_| cx.throw_range_error("`limit` must fit in u16"))
        })
        .transpose()?;
    let offset: Option<u16> = js_value_to_option::<JsNumber, _>(js_object.get(cx, "offset")?, cx)?
        .map(|x| {
            u16::try_from(x.value(cx) as i64)
                .or_else(|_| cx.throw_range_error("`offset` must fit in u16"))
        })
        .transpose()?;
    Ok(SizedQuery::new(query, limit, offset))
}

/// Convert js path query to path query
pub fn js_path_query_to_path_query<'a, C: Context<'a>>(
    js_path_query: Handle<JsObject>,
    cx: &mut C,
) -> NeonResult<PathQuery> {
    let path = js_array_of_buffers_to_vec(js_path_query.get(cx, "path")?, cx)?;
    let query = js_object_to_sized_query(js_path_query.get(cx, "query")?, cx)?;
    Ok(PathQuery::new(path, query))
}
