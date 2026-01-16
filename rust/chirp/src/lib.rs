mod built_info {
    include!(concat!(env!("OUT_DIR"), "/built.rs"));
}

#[cxx::bridge(namespace = "chirp")]
mod ffi {
    extern "Rust" {
        fn chirp() -> String;
    }
}

fn chirp() -> String {
    format!(
        "{} {} built with {} reports \"cheep cheep\"",
        built_info::PKG_NAME,
        built_info::PKG_VERSION,
        built_info::RUSTC_VERSION,
    )
}
