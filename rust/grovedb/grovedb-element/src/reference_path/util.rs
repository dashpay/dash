use crate::hex_to_ascii;

pub fn path_as_slices_hex_to_ascii(path: &[&[u8]]) -> String {
    path.iter()
        .map(|e| hex_to_ascii(e))
        .collect::<Vec<_>>()
        .join("/")
}
