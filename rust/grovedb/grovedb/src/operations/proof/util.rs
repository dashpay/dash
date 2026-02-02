use std::fmt;

use grovedb_merk::{
    proofs::query::{Key, Path, ProvedKeyOptionalValue, ProvedKeyValue},
    CryptoHash, Error,
};
use grovedb_version::version::GroveVersion;

use crate::Element;

#[cfg(any(feature = "minimal", feature = "verify"))]
pub type ProvedKeyValues = Vec<ProvedKeyValue>;

#[cfg(any(feature = "minimal", feature = "verify"))]
pub type ProvedKeyOptionalValues = Vec<ProvedKeyOptionalValue>;

#[cfg(any(feature = "minimal", feature = "verify"))]
pub type ProvedPathKeyValues = Vec<ProvedPathKeyValue>;

#[cfg(any(feature = "minimal", feature = "verify"))]
pub type ProvedPathKeyOptionalValues = Vec<ProvedPathKeyOptionalValue>;

/// Proved path-key-value
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, PartialEq, Eq)]
pub struct ProvedPathKeyOptionalValue {
    /// Path
    pub path: Path,
    /// Key
    pub key: Key,
    /// Value
    pub value: Option<Vec<u8>>,
    /// Proof
    pub proof: CryptoHash,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl fmt::Display for ProvedPathKeyOptionalValue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "ProvedPathKeyValue {{")?;
        writeln!(
            f,
            "  path: [{}],",
            self.path
                .iter()
                .map(|p| hex_to_ascii(p))
                .collect::<Vec<_>>()
                .join(", ")
        )?;
        writeln!(f, "  key: {},", hex_to_ascii(&self.key))?;
        writeln!(
            f,
            "  value: {},",
            optional_element_hex_to_ascii(self.value.as_ref())
        )?;
        writeln!(f, "  proof: {}", hex::encode(self.proof))?;
        write!(f, "}}")
    }
}

/// Proved path-key-value
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, PartialEq, Eq)]
pub struct ProvedPathKeyValue {
    /// Path
    pub path: Path,
    /// Key
    pub key: Key,
    /// Value
    pub value: Vec<u8>,
    /// Proof
    pub proof: CryptoHash,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl fmt::Display for ProvedPathKeyValue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "ProvedPathKeyValue {{")?;
        writeln!(
            f,
            "  path: [{}],",
            self.path
                .iter()
                .map(|p| hex_to_ascii(p))
                .collect::<Vec<_>>()
                .join(", ")
        )?;
        writeln!(f, "  key: {},", hex_to_ascii(&self.key))?;
        writeln!(f, "  value: {},", element_hex_to_ascii(self.value.as_ref()))?;
        writeln!(f, "  proof: {}", hex::encode(self.proof))?;
        write!(f, "}}")
    }
}

impl From<ProvedPathKeyValue> for ProvedPathKeyOptionalValue {
    fn from(value: ProvedPathKeyValue) -> Self {
        let ProvedPathKeyValue {
            path,
            key,
            value,
            proof,
        } = value;

        ProvedPathKeyOptionalValue {
            path,
            key,
            value: Some(value),
            proof,
        }
    }
}

impl TryFrom<ProvedPathKeyOptionalValue> for ProvedPathKeyValue {
    type Error = Error;

    fn try_from(value: ProvedPathKeyOptionalValue) -> Result<Self, Self::Error> {
        let ProvedPathKeyOptionalValue {
            path,
            key,
            value,
            proof,
        } = value;
        let value = value.ok_or(Error::InvalidProofError(format!(
            "expected {}",
            hex_to_ascii(&key)
        )))?;
        Ok(ProvedPathKeyValue {
            path,
            key,
            value,
            proof,
        })
    }
}

impl ProvedPathKeyValue {
    // TODO: make path a reference
    /// Consumes the ProvedKeyValue and returns a ProvedPathKeyValue given a
    /// Path
    pub fn from_proved_key_value(path: Path, proved_key_value: ProvedKeyValue) -> Self {
        Self {
            path,
            key: proved_key_value.key,
            value: proved_key_value.value,
            proof: proved_key_value.proof,
        }
    }

    /// Transforms multiple ProvedKeyValues to their equivalent
    /// ProvedPathKeyValue given a Path
    pub fn from_proved_key_values(path: Path, proved_key_values: ProvedKeyValues) -> Vec<Self> {
        proved_key_values
            .into_iter()
            .map(|pkv| Self::from_proved_key_value(path.clone(), pkv))
            .collect()
    }
}

impl ProvedPathKeyOptionalValue {
    // TODO: make path a reference
    /// Consumes the ProvedKeyValue and returns a ProvedPathKeyValue given a
    /// Path
    pub fn from_proved_key_value(path: Path, proved_key_value: ProvedKeyOptionalValue) -> Self {
        Self {
            path,
            key: proved_key_value.key,
            value: proved_key_value.value,
            proof: proved_key_value.proof,
        }
    }

    /// Transforms multiple ProvedKeyValues to their equivalent
    /// ProvedPathKeyValue given a Path
    pub fn from_proved_key_values(
        path: Path,
        proved_key_values: ProvedKeyOptionalValues,
    ) -> Vec<Self> {
        proved_key_values
            .into_iter()
            .map(|pkv| Self::from_proved_key_value(path.clone(), pkv))
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use grovedb_merk::proofs::query::ProvedKeyOptionalValue;

    use crate::operations::proof::util::ProvedPathKeyOptionalValue;

    #[test]
    fn test_proved_path_from_single_proved_key_value() {
        let path = vec![b"1".to_vec(), b"2".to_vec()];
        let proved_key_value = ProvedKeyOptionalValue {
            key: b"a".to_vec(),
            value: Some(vec![5, 6]),
            proof: [0; 32],
        };
        let proved_path_key_value =
            ProvedPathKeyOptionalValue::from_proved_key_value(path.clone(), proved_key_value);
        assert_eq!(
            proved_path_key_value,
            ProvedPathKeyOptionalValue {
                path,
                key: b"a".to_vec(),
                value: Some(vec![5, 6]),
                proof: [0; 32]
            }
        );
    }

    #[test]
    fn test_many_proved_path_from_many_proved_key_value() {
        let path = vec![b"1".to_vec(), b"2".to_vec()];
        let proved_key_value_a = ProvedKeyOptionalValue {
            key: b"a".to_vec(),
            value: Some(vec![5, 6]),
            proof: [0; 32],
        };
        let proved_key_value_b = ProvedKeyOptionalValue {
            key: b"b".to_vec(),
            value: Some(vec![5, 7]),
            proof: [1; 32],
        };
        let proved_key_value_c = ProvedKeyOptionalValue {
            key: b"c".to_vec(),
            value: Some(vec![6, 7]),
            proof: [2; 32],
        };
        let proved_key_value_d = ProvedKeyOptionalValue {
            key: b"d".to_vec(),
            value: None,
            proof: [2; 32],
        };
        let proved_key_values = vec![
            proved_key_value_a,
            proved_key_value_b,
            proved_key_value_c,
            proved_key_value_d,
        ];
        let proved_path_key_values =
            ProvedPathKeyOptionalValue::from_proved_key_values(path.clone(), proved_key_values);
        assert_eq!(proved_path_key_values.len(), 4);
        assert_eq!(
            proved_path_key_values[0],
            ProvedPathKeyOptionalValue {
                path: path.clone(),
                key: b"a".to_vec(),
                value: Some(vec![5, 6]),
                proof: [0; 32]
            }
        );
        assert_eq!(
            proved_path_key_values[1],
            ProvedPathKeyOptionalValue {
                path: path.clone(),
                key: b"b".to_vec(),
                value: Some(vec![5, 7]),
                proof: [1; 32]
            }
        );
        assert_eq!(
            proved_path_key_values[2],
            ProvedPathKeyOptionalValue {
                path: path.clone(),
                key: b"c".to_vec(),
                value: Some(vec![6, 7]),
                proof: [2; 32]
            }
        );

        assert_eq!(
            proved_path_key_values[3],
            ProvedPathKeyOptionalValue {
                path,
                key: b"d".to_vec(),
                value: None,
                proof: [2; 32]
            }
        );
    }
}

pub fn hex_to_ascii(hex_value: &[u8]) -> String {
    // Define the set of allowed characters
    const ALLOWED_CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ\
                                  abcdefghijklmnopqrstuvwxyz\
                                  0123456789_-/\\[]@";

    // Check if all characters in hex_value are allowed
    if hex_value.iter().all(|&c| ALLOWED_CHARS.contains(&c)) {
        // Try to convert to UTF-8
        String::from_utf8(hex_value.to_vec())
            .unwrap_or_else(|_| format!("0x{}", hex::encode(hex_value)))
    } else {
        // Hex encode and prepend "0x"
        format!("0x{}", hex::encode(hex_value))
    }
}

pub fn path_hex_to_ascii(path: &Path) -> String {
    path.iter()
        .map(|e| hex_to_ascii(e.as_slice()))
        .collect::<Vec<_>>()
        .join("/")
}

pub fn path_as_slices_hex_to_ascii(path: &[&[u8]]) -> String {
    path.iter()
        .map(|e| hex_to_ascii(e))
        .collect::<Vec<_>>()
        .join("/")
}
pub fn optional_element_hex_to_ascii(hex_value: Option<&Vec<u8>>) -> String {
    match hex_value {
        None => "None".to_string(),
        Some(hex_value) => Element::deserialize(hex_value, GroveVersion::latest())
            .map(|e| e.to_string())
            .unwrap_or_else(|_| hex::encode(hex_value)),
    }
}

pub fn element_hex_to_ascii(hex_value: &[u8]) -> String {
    Element::deserialize(hex_value, GroveVersion::latest())
        .map(|e| e.to_string())
        .unwrap_or_else(|_| hex::encode(hex_value))
}
