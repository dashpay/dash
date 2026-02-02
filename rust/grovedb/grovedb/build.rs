#[cfg(feature = "grovedbg")]
fn main() {
    use std::{env, fs::File, io::Cursor, path::PathBuf};

    use hex_literal::hex;
    use sha2::{digest::FixedOutput, Digest, Sha256};

    const GROVEDBG_SHA256: [u8; 32] =
        hex!("da31eb2c93d553abb730455b53761a6e3913d876d0371b4a4a7e08a7398322ed");
    const GROVEDBG_VERSION: &str = "v1.2.0";

    let out_dir = PathBuf::from(&env::var_os("OUT_DIR").unwrap());
    let grovedbg_zip_path = out_dir.join("grovedbg.zip");

    if !grovedbg_zip_path.exists() {
        let response = reqwest::blocking::get(format!(
            "https://github.com/dashpay/grovedbg/releases/download/\
{GROVEDBG_VERSION}/grovedbg-{GROVEDBG_VERSION}.zip"
        ))
        .expect("can't download GroveDBG artifact");

        let mut grovedbg_zip = File::create(&grovedbg_zip_path).unwrap();
        let mut content = Cursor::new(response.bytes().unwrap());
        std::io::copy(&mut content, &mut grovedbg_zip).unwrap();
    }

    let mut grovedbg_zip = File::open(&grovedbg_zip_path).unwrap();

    let mut sha256 = Sha256::new();
    std::io::copy(&mut grovedbg_zip, &mut sha256).unwrap();
    let hash = sha256.finalize_fixed();

    assert_eq!(hash.as_slice(), GROVEDBG_SHA256);
}

#[cfg(not(feature = "grovedbg"))]
fn main() {}
