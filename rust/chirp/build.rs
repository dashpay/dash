fn main() {
    built::write_built_file().expect("Failed to write build info");
    cxx_build::bridge("src/lib.rs").compile("chirp");
    println!("cargo::rerun-if-changed=src/lib.rs");
}
