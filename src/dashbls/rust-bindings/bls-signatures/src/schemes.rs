use std::ffi::{c_void, CStr};

use bls_dash_sys::{
    AugSchemeMPLAggregateVerify, AugSchemeMPLFree, AugSchemeMPLSign, AugSchemeMPLVerify,
    BasicSchemeMPLAggregateVerify, BasicSchemeMPLFree, CoreMPLAggregatePubKeys,
    CoreMPLAggregateSigs, CoreMPLSign, CoreMPLVerify, CoreMPLVerifySecure, GetLastErrorMsg,
    LegacySchemeMPLAggregateVerify, LegacySchemeMPLSign, LegacySchemeMPLVerify,
    LegacySchemeMPLVerifySecure, NewAugSchemeMPL, NewBasicSchemeMPL, NewLegacySchemeMPL,
};

// TODO Split into modules
use crate::{private_key::PrivateKey, G1Element, G2Element};

pub trait Scheme {
    fn as_mut_ptr(&self) -> *mut c_void;

    fn sign(&self, private_key: &PrivateKey, message: &[u8]) -> G2Element;

    fn verify(&self, public_key: &G1Element, message: &[u8], signature: &G2Element) -> bool;

    fn verify_secure<'a>(
        &self,
        public_keys: impl IntoIterator<Item = &'a G1Element>,
        message: &[u8],
        signature: &G2Element,
    ) -> bool {
        let mut g1_pointers = public_keys
            .into_iter()
            .map(|g1| g1.c_element)
            .collect::<Vec<_>>();

        unsafe {
            CoreMPLVerifySecure(
                self.as_mut_ptr(),
                g1_pointers.as_mut_ptr(),
                g1_pointers.len(),
                signature.c_element,
                message.as_ptr() as *const _,
                message.len(),
            )
        }
    }

    fn aggregate_public_keys<'a>(
        &self,
        public_keys: impl IntoIterator<Item = &'a G1Element>,
    ) -> G1Element {
        let mut g1_pointers = public_keys
            .into_iter()
            .map(|g1| g1.c_element)
            .collect::<Vec<_>>();
        G1Element {
            c_element: unsafe {
                CoreMPLAggregatePubKeys(
                    self.as_mut_ptr(),
                    g1_pointers.as_mut_ptr(),
                    g1_pointers.len(),
                )
            },
        }
    }

    fn aggregate_sigs<'a>(&self, sigs: impl IntoIterator<Item = &'a G2Element>) -> G2Element {
        let mut g2_pointers = sigs.into_iter().map(|g2| g2.c_element).collect::<Vec<_>>();
        G2Element {
            c_element: unsafe {
                CoreMPLAggregateSigs(
                    self.as_mut_ptr(),
                    g2_pointers.as_mut_ptr(),
                    g2_pointers.len(),
                )
            },
        }
    }

    fn aggregate_verify<'a>(
        &self,
        public_keys: impl IntoIterator<Item = &'a G1Element>,
        messages: impl IntoIterator<Item = &'a [u8]>,
        signature: &G2Element,
    ) -> bool;
}

struct AggregateVerifyArgs {
    g1_pointers: Vec<*mut c_void>,
    messages_pointers: Vec<*const u8>,
    messages_lengths: Vec<usize>,
}

// TODO put constructor inside struct?
fn prepare_aggregate_verify_args<'a>(
    public_keys: impl IntoIterator<Item = &'a G1Element>,
    messages: impl IntoIterator<Item = &'a [u8]>,
) -> AggregateVerifyArgs {
    let g1_pointers = public_keys
        .into_iter()
        .map(|g1| g1.c_element)
        .collect::<Vec<_>>();

    let mut messages_pointers = Vec::new();
    let mut messages_lengths = Vec::new();

    for m in messages.into_iter() {
        messages_pointers.push(m.as_ptr());
        messages_lengths.push(m.len());
    }

    AggregateVerifyArgs {
        g1_pointers,
        messages_pointers,
        messages_lengths,
    }
}

pub struct BasicSchemeMPL {
    scheme: *mut c_void,
}

impl BasicSchemeMPL {
    pub fn new() -> Self {
        BasicSchemeMPL {
            scheme: unsafe { NewBasicSchemeMPL() },
        }
    }
}

impl Scheme for BasicSchemeMPL {
    fn as_mut_ptr(&self) -> *mut c_void {
        self.scheme
    }

    fn sign(&self, private_key: &PrivateKey, message: &[u8]) -> G2Element {
        G2Element {
            c_element: unsafe {
                CoreMPLSign(
                    self.scheme,
                    private_key.as_mut_ptr(),
                    message.as_ptr() as *const _,
                    message.len(),
                )
            },
        }
    }

    fn verify(&self, public_key: &G1Element, message: &[u8], signature: &G2Element) -> bool {
        unsafe {
            CoreMPLVerify(
                self.scheme,
                public_key.c_element,
                message.as_ptr() as *const _,
                message.len(),
                signature.c_element,
            )
        }
    }

    fn aggregate_verify<'a>(
        &self,
        public_keys: impl IntoIterator<Item = &'a G1Element>,
        messages: impl IntoIterator<Item = &'a [u8]>,
        signature: &G2Element,
    ) -> bool {
        let AggregateVerifyArgs {
            mut g1_pointers,
            mut messages_pointers,
            messages_lengths: mut messages_lengthes,
        } = prepare_aggregate_verify_args(public_keys, messages);

        unsafe {
            BasicSchemeMPLAggregateVerify(
                self.as_mut_ptr(),
                g1_pointers.as_mut_ptr(),
                g1_pointers.len(),
                messages_pointers.as_mut_ptr() as *mut _,
                messages_lengthes.as_mut_ptr() as *mut _,
                messages_pointers.len(),
                signature.c_element,
            )
        }
    }
}

pub struct LegacySchemeMPL {
    scheme: *mut c_void,
}

impl LegacySchemeMPL {
    pub fn new() -> Self {
        LegacySchemeMPL {
            scheme: unsafe { NewLegacySchemeMPL() },
        }
    }
}

impl Scheme for LegacySchemeMPL {
    fn as_mut_ptr(&self) -> *mut c_void {
        self.scheme
    }

    fn sign(&self, private_key: &PrivateKey, message: &[u8]) -> G2Element {
        // The legacy scheme signs 32-byte message hashes; the C++ side throws
        // for anything else, and a C++ exception must not cross the FFI.
        assert_eq!(message.len(), 32, "the legacy scheme signs a 32-byte message hash");
        let c_element = unsafe {
            LegacySchemeMPLSign(
                self.scheme,
                private_key.as_mut_ptr(),
                message.as_ptr() as *const _,
                message.len(),
            )
        };
        // The C shim returns null when the C++ side threw.
        if c_element.is_null() {
            let msg = unsafe { CStr::from_ptr(GetLastErrorMsg()) };
            panic!("LegacySchemeMPLSign failed: {}", msg.to_string_lossy());
        }
        G2Element { c_element }
    }

    fn verify(&self, public_key: &G1Element, message: &[u8], signature: &G2Element) -> bool {
        unsafe {
            LegacySchemeMPLVerify(
                self.scheme,
                public_key.c_element,
                message.as_ptr() as *const _,
                message.len(),
                signature.c_element,
            )
        }
    }

    fn verify_secure<'a>(
        &self,
        public_keys: impl IntoIterator<Item = &'a G1Element>,
        message: &[u8],
        signature: &G2Element,
    ) -> bool {
        let mut g1_pointers = public_keys
            .into_iter()
            .map(|g1| g1.c_element)
            .collect::<Vec<_>>();

        unsafe {
            LegacySchemeMPLVerifySecure(
                self.as_mut_ptr(),
                g1_pointers.as_mut_ptr(),
                g1_pointers.len(),
                signature.c_element,
                message.as_ptr() as *const _,
                message.len(),
            )
        }
    }

    fn aggregate_verify<'a>(
        &self,
        public_keys: impl IntoIterator<Item = &'a G1Element>,
        messages: impl IntoIterator<Item = &'a [u8]>,
        signature: &G2Element,
    ) -> bool {
        let AggregateVerifyArgs {
            mut g1_pointers,
            mut messages_pointers,
            messages_lengths: mut messages_lengthes,
        } = prepare_aggregate_verify_args(public_keys, messages);

        unsafe {
            LegacySchemeMPLAggregateVerify(
                self.as_mut_ptr(),
                g1_pointers.as_mut_ptr(),
                g1_pointers.len(),
                messages_pointers.as_mut_ptr() as *mut _,
                messages_lengthes.as_mut_ptr() as *mut _,
                messages_pointers.len(),
                signature.c_element,
            )
        }
    }
}

impl Drop for BasicSchemeMPL {
    fn drop(&mut self) {
        unsafe { BasicSchemeMPLFree(self.scheme) }
    }
}

pub struct AugSchemeMPL {
    scheme: *mut c_void,
}

impl AugSchemeMPL {
    pub fn new() -> Self {
        AugSchemeMPL {
            scheme: unsafe { NewAugSchemeMPL() },
        }
    }
}

impl Scheme for AugSchemeMPL {
    fn as_mut_ptr(&self) -> *mut c_void {
        self.scheme
    }

    fn sign(&self, private_key: &PrivateKey, message: &[u8]) -> G2Element {
        G2Element {
            c_element: unsafe {
                AugSchemeMPLSign(
                    self.scheme,
                    private_key.as_mut_ptr(),
                    message.as_ptr() as *const _,
                    message.len(),
                )
            },
        }
    }

    fn verify(&self, public_key: &G1Element, message: &[u8], signature: &G2Element) -> bool {
        unsafe {
            AugSchemeMPLVerify(
                self.scheme,
                public_key.c_element,
                message.as_ptr() as *const _,
                message.len(),
                signature.c_element,
            )
        }
    }

    fn aggregate_verify<'a>(
        &self,
        public_keys: impl IntoIterator<Item = &'a G1Element>,
        messages: impl IntoIterator<Item = &'a [u8]>,
        signature: &G2Element,
    ) -> bool {
        let AggregateVerifyArgs {
            mut g1_pointers,
            mut messages_pointers,
            mut messages_lengths,
        } = prepare_aggregate_verify_args(public_keys, messages);

        unsafe {
            AugSchemeMPLAggregateVerify(
                self.as_mut_ptr(),
                g1_pointers.as_mut_ptr(),
                g1_pointers.len(),
                messages_pointers.as_mut_ptr() as *mut _,
                messages_lengths.as_mut_ptr() as *mut _,
                messages_pointers.len(),
                signature.c_element,
            )
        }
    }
}

impl Drop for AugSchemeMPL {
    fn drop(&mut self) {
        unsafe { AugSchemeMPLFree(self.scheme) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn verify_aggregate(scheme: impl Scheme) {
        let seed1 = b"seedweedseedweedseedweedseedweed";
        let seed2 = b"weedseedweedseedweedseedweedseed";
        let seed3 = b"seedseedseedseedweedweedweedweed";
        let seed4 = b"weedweedweedweedweedweedweedweed";

        let private_key_1 =
            PrivateKey::key_gen(&scheme, seed1).expect("unable to generate private key");
        let private_key_2 =
            PrivateKey::key_gen(&scheme, seed2).expect("unable to generate private key");
        let private_key_3 =
            PrivateKey::key_gen(&scheme, seed3).expect("unable to generate private key");
        let private_key_4 =
            PrivateKey::key_gen(&scheme, seed4).expect("unable to generate private key");

        let public_key_1 = private_key_1
            .g1_element()
            .expect("unable to get public key");
        let public_key_2 = private_key_2
            .g1_element()
            .expect("unable to get public key");
        let public_key_3 = private_key_3
            .g1_element()
            .expect("unable to get public key");
        let public_key_4 = private_key_4
            .g1_element()
            .expect("unable to get public key");

        // 32-byte messages: the legacy scheme only signs message hashes
        let message_1 = &[0x11u8; 32];
        let message_2 = &[0x22u8; 32];
        let message_3 = &[0x33u8; 32];
        let message_4 = &[0x44u8; 32];

        let signature_1 = scheme.sign(&private_key_1, message_1);
        let signature_2 = scheme.sign(&private_key_2, message_2);
        let signature_3 = scheme.sign(&private_key_3, message_3);
        let signature_4 = scheme.sign(&private_key_4, message_4);

        let signature_agg = scheme.aggregate_sigs([&signature_1, &signature_2, &signature_3]);

        let verify = scheme.aggregate_verify(
            [&public_key_1, &public_key_2, &public_key_3],
            [message_1.as_ref(), message_2.as_ref(), message_3.as_ref()],
            &signature_agg,
        );
        assert!(verify);

        // Arbitrary trees of aggregates
        let signature_agg_final = scheme.aggregate_sigs([&signature_agg, &signature_4]);
        let verify_final = scheme.aggregate_verify(
            [&public_key_1, &public_key_2, &public_key_3, &public_key_4],
            [
                message_1.as_ref(),
                message_2.as_ref(),
                message_3.as_ref(),
                message_4.as_ref(),
            ],
            &signature_agg_final,
        );
        assert!(verify_final);
    }

    #[test]
    fn verify_aggregate_aug() {
        verify_aggregate(AugSchemeMPL::new());
    }

    #[test]
    fn verify_aggregate_basic() {
        verify_aggregate(BasicSchemeMPL::new());
    }

    #[test]
    fn verify_aggregate_legacy() {
        verify_aggregate(LegacySchemeMPL::new());
    }

    fn legacy_key_and_signature() -> (LegacySchemeMPL, G1Element, G2Element) {
        let scheme = LegacySchemeMPL::new();
        let private_key = PrivateKey::key_gen(&scheme, b"seedweedseedweedseedweedseedweed")
            .expect("unable to generate private key");
        let public_key = private_key.g1_element().expect("unable to get public key");
        let signature = scheme.sign(&private_key, &[0x11u8; 32]);
        (scheme, public_key, signature)
    }

    #[test]
    #[should_panic(expected = "32-byte message hash")]
    fn legacy_sign_rejects_short_message() {
        let scheme = LegacySchemeMPL::new();
        let private_key = PrivateKey::key_gen(&scheme, b"seedweedseedweedseedweedseedweed")
            .expect("unable to generate private key");
        scheme.sign(&private_key, &[0x11u8; 31]);
    }

    #[test]
    #[should_panic(expected = "32-byte message hash")]
    fn legacy_sign_rejects_long_message() {
        let scheme = LegacySchemeMPL::new();
        let private_key = PrivateKey::key_gen(&scheme, b"seedweedseedweedseedweedseedweed")
            .expect("unable to generate private key");
        scheme.sign(&private_key, &[0x11u8; 33]);
    }

    #[test]
    fn legacy_verify_rejects_wrong_message_length() {
        let (scheme, public_key, signature) = legacy_key_and_signature();
        assert!(scheme.verify(&public_key, &[0x11u8; 32], &signature));
        for message in [&[0x11u8; 31][..], &[0x11u8; 33][..]] {
            assert!(!scheme.verify(&public_key, message, &signature));
            assert!(!scheme.verify_secure([&public_key], message, &signature));
            assert!(!scheme.aggregate_verify([&public_key], [message], &signature));
        }
    }
}
