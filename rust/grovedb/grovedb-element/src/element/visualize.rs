//! Visualize

use std::{
    fmt,
    io::{Result, Write},
};

use grovedb_visualize::{Drawer, Visualize};

use crate::{element::Element, visualize_helpers::visualize_to_vec};

impl Visualize for Element {
    fn visualize<W: Write>(&self, mut drawer: Drawer<W>) -> Result<Drawer<W>> {
        match self {
            Element::Item(value, flags) => {
                drawer.write(b"item: ")?;
                drawer = value.visualize(drawer)?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
            Element::SumItem(value, flags) => {
                drawer.write(format!("sum_item: {value}").as_bytes())?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
            Element::Reference(_ref, ..) => {
                drawer.write(b"ref")?;
                // drawer.write(b"ref: [path: ")?;
                // let mut path_iter = path.iter();
                // if let Some(first) = path_iter.next() {
                //     drawer = first.visualize(drawer)?;
                // }
                // for p in path_iter {
                //     drawer.write(b", ")?;
                //     drawer = p.visualize(drawer)?;
                // }
                // drawer.write(b"]")?;
            }
            Element::Tree(root_key, flags) => {
                drawer.write(b"tree: ")?;
                drawer = root_key.as_deref().visualize(drawer)?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
            Element::SumTree(root_key, value, flags) => {
                drawer.write(b"sum_tree: ")?;
                drawer = root_key.as_deref().visualize(drawer)?;
                drawer.write(format!(" {value}").as_bytes())?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
            Element::BigSumTree(root_key, value, flags) => {
                drawer.write(b"big_sum_tree: ")?;
                drawer = root_key.as_deref().visualize(drawer)?;
                drawer.write(format!(" {value}").as_bytes())?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
            Element::CountTree(root_key, value, flags) => {
                drawer.write(b"count_tree: ")?;
                drawer = root_key.as_deref().visualize(drawer)?;
                drawer.write(format!(" {value}").as_bytes())?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
            Element::CountSumTree(root_key, count_value, sum_value, flags) => {
                drawer.write(b"count_sum_tree: ")?;
                drawer = root_key.as_deref().visualize(drawer)?;
                drawer.write(format!("count: {count_value}, sum {sum_value}").as_bytes())?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }

            Element::ProvableCountTree(root_key, value, flags) => {
                drawer.write(b"provable_count_tree: ")?;
                drawer = root_key.as_deref().visualize(drawer)?;
                drawer.write(format!(" {value}").as_bytes())?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
            Element::ProvableCountSumTree(root_key, count_value, sum_value, flags) => {
                drawer.write(b"provable_count_sum_tree: ")?;
                drawer = root_key.as_deref().visualize(drawer)?;
                drawer.write(format!("count: {count_value}, sum {sum_value}").as_bytes())?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
            Element::ItemWithSumItem(value, sum_value, flags) => {
                drawer.write(b"item_with_sum_item: ")?;
                drawer = value.visualize(drawer)?;
                drawer.write(format!(" {sum_value}").as_bytes())?;

                if let Some(f) = flags {
                    if !f.is_empty() {
                        drawer = f.visualize(drawer)?;
                    }
                }
            }
        }
        Ok(drawer)
    }
}

impl fmt::Debug for Element {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut v = Vec::new();
        visualize_to_vec(&mut v, self);

        f.write_str(&String::from_utf8_lossy(&v))
    }
}

#[cfg(test)]
mod tests {
    use grovedb_visualize::to_hex;

    use super::*;
    use crate::reference_path::ReferencePathType;

    #[test]
    fn test_element_item_str() {
        let v = b"ayylmao".to_vec();
        let e = Element::new_item(v.clone());
        let element_hex = to_hex(&v);
        let mut result = Vec::new();
        let drawer = Drawer::new(&mut result);
        e.visualize(drawer).expect("visualize IO error");
        assert_eq!(
            format!(
                "item: [hex: {element_hex}, str: {}]",
                String::from_utf8_lossy(&v)
            ),
            String::from_utf8_lossy(result.as_ref())
        );
    }

    #[test]
    fn test_element_item_no_tr() {
        let v = vec![1, 3, 3, 7, 255];
        let e = Element::new_item(v.clone());
        let element_hex = to_hex(&v);
        let mut result = Vec::new();
        let drawer = Drawer::new(&mut result);
        e.visualize(drawer).expect("visualize IO error");
        assert_eq!(
            format!("item: [hex: {element_hex}]"),
            String::from_utf8_lossy(result.as_ref())
        );
    }

    #[test]
    #[ignore]
    fn test_visualize_reference() {
        let p1 = b"ayy".to_vec();
        let p2 = b"lmao".to_vec();
        let e = Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
            p1.clone(),
            p2.clone(),
        ]));
        let mut result = Vec::new();
        let drawer = Drawer::new(&mut result);
        e.visualize(drawer).expect("visualize IO error");
        assert_eq!(
            format!(
                "ref: [path: [hex: {p1_hex}, str: {p1}], [hex: {p2_hex}, str: {p2}]]",
                p1 = String::from_utf8_lossy(&p1),
                p2 = String::from_utf8_lossy(&p2),
                p1_hex = to_hex(&p1),
                p2_hex = to_hex(&p2),
            ),
            String::from_utf8_lossy(result.as_ref())
        );
    }
}
