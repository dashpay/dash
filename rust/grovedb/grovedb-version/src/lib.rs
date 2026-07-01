use version::GroveVersion;

pub mod error;
pub mod version;

#[macro_export]
macro_rules! check_grovedb_v0_with_cost {
    ($method:expr, $version:expr) => {{
        const EXPECTED_VERSION: u16 = 0;
        if $version != EXPECTED_VERSION {
            return grovedb_costs::CostsExt::wrap_with_cost(
                Err($crate::error::GroveVersionError::UnknownVersionMismatch {
                    method: $method.to_string(),
                    known_versions: vec![EXPECTED_VERSION],
                    received: $version,
                }
                .into()),
                Default::default(),
            );
        }
    }};
}

#[macro_export]
macro_rules! check_grovedb_v0 {
    ($method:expr, $version:expr) => {{
        const EXPECTED_VERSION: u16 = 0;
        if $version != EXPECTED_VERSION {
            return Err($crate::error::GroveVersionError::UnknownVersionMismatch {
                method: $method.to_string(),
                known_versions: vec![EXPECTED_VERSION],
                received: $version,
            }
            .into());
        }
    }};
}

#[macro_export]
macro_rules! check_grovedb_v0_or_v1 {
    ($method:expr, $version:expr) => {{
        const EXPECTED_VERSION_V0: u16 = 0;
        const EXPECTED_VERSION_V1: u16 = 1;
        if $version != EXPECTED_VERSION_V0 && $version != EXPECTED_VERSION_V1 {
            return Err(GroveVersionError::UnknownVersionMismatch {
                method: $method.to_string(),
                known_versions: vec![EXPECTED_VERSION_V0, EXPECTED_VERSION_V1],
                received: $version,
            }
            .into());
        }
        $version
    }};
}

#[macro_export]
macro_rules! check_merk_v0_with_cost {
    ($method:expr, $version:expr) => {{
        const EXPECTED_VERSION: u16 = 0;
        if $version != EXPECTED_VERSION {
            return grovedb_costs::CostsExt::wrap_with_cost(
                Err($crate::error::GroveVersionError::UnknownVersionMismatch {
                    method: $method.to_string(),
                    known_versions: vec![EXPECTED_VERSION],
                    received: $version,
                }
                .into()),
                Default::default(),
            );
        }
    }};
}

#[macro_export]
macro_rules! check_merk_v0 {
    ($method:expr, $version:expr) => {{
        const EXPECTED_VERSION: u16 = 0;
        if $version != EXPECTED_VERSION {
            return Err($crate::error::GroveVersionError::UnknownVersionMismatch {
                method: $method.to_string(),
                known_versions: vec![EXPECTED_VERSION],
                received: $version,
            }
            .into());
        }
    }};
}

pub trait TryFromVersioned<T>: Sized {
    /// The type returned in the event of a conversion error.
    type Error;

    /// Performs the conversion.
    fn try_from_versioned(value: T, grove_version: &GroveVersion) -> Result<Self, Self::Error>;
}

pub trait TryIntoVersioned<T>: Sized {
    /// The type returned in the event of a conversion error.
    type Error;

    /// Performs the conversion.
    fn try_into_versioned(self, grove_version: &GroveVersion) -> Result<T, Self::Error>;
}

impl<T, U> TryIntoVersioned<U> for T
where
    U: TryFromVersioned<T>,
{
    type Error = U::Error;

    #[inline]
    fn try_into_versioned(self, grove_version: &GroveVersion) -> Result<U, U::Error> {
        U::try_from_versioned(self, grove_version)
    }
}

impl<T, U> TryFromVersioned<U> for T
where
    T: TryFrom<U>,
{
    type Error = T::Error;

    #[inline]
    fn try_from_versioned(value: U, _grove_version: &GroveVersion) -> Result<Self, Self::Error> {
        T::try_from(value)
    }
}
