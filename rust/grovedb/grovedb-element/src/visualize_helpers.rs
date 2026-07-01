use grovedb_visualize::{Drawer, Visualize};

/// `visualize` shortcut to write into provided buffer, should be a `Vec` not a
/// slice because slices won't grow if needed.
pub fn visualize_to_vec<T: Visualize + ?Sized>(v: &mut Vec<u8>, value: &T) {
    let drawer = Drawer::new(v);
    value
        .visualize(drawer)
        .expect("error while writing into slice");
}
