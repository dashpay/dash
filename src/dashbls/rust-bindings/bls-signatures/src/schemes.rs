use std::ffi::c_void;

use bls_dash_sys::{
    AugSchemeMPLAggregateVerify, AugSchemeMPLFree, AugSchemeMPLSign, AugSchemeMPLVerify,
    BasicSchemeMPLAggregateVerify, BasicSchemeMPLFree, CoreMPLAggregatePubKeys,
    CoreMPLAggregateSigs, CoreMPLSign, CoreMPLVerify, CoreMPLVerifySecure,
    LegacySchemeMPLAggregateVerify, LegacySchemeMPLSign, LegacySchemeMPLVerify,
    LegacySchemeMPLVerifySecure, NewAugSchemeMPL, NewBasicSchemeMPL, NewLegacySchemeMPL,
};

// TODO Split into modules
use crate::{private_key::PrivateKey, utils::c_err_to_result, BlsError, G1Element, G2Element};

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
        let c_element = c_err_to_result(|did_error| {
            let result = unsafe {
                CoreMPLSign(
                    self.scheme,
                    private_key.as_mut_ptr(),
                    message.as_ptr() as *const _,
                    message.len(),
                )
            };
            *did_error = result.is_null();
            result
        })
        .expect("Signing failed");
        G2Element { c_element }
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

    /// Signs the first 32 bytes of a legacy message, rejecting shorter messages.
    pub fn try_sign(
        &self,
        private_key: &PrivateKey,
        message: &[u8],
    ) -> Result<G2Element, BlsError> {
        if message.len() < 32 {
            return Err(BlsError {
                msg: "Legacy message must contain at least 32 bytes".to_owned(),
            });
        }
        let c_element = c_err_to_result(|did_error| {
            let result = unsafe {
                LegacySchemeMPLSign(
                    self.scheme,
                    private_key.as_mut_ptr(),
                    message.as_ptr() as *const _,
                    message.len(),
                )
            };
            *did_error = result.is_null();
            result
        })?;
        Ok(G2Element { c_element })
    }
}

impl Scheme for LegacySchemeMPL {
    fn as_mut_ptr(&self) -> *mut c_void {
        self.scheme
    }

    /// Panics if legacy signing fails. Use `try_sign` for a recoverable error.
    fn sign(&self, private_key: &PrivateKey, message: &[u8]) -> G2Element {
        self.try_sign(private_key, message)
            .expect("Legacy signing failed")
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

    fn verify_aggregate(scheme: impl Scheme, message_repetitions: usize) {
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

        let message_1 = b"ayya".repeat(message_repetitions);
        let message_2 = b"ayyb".repeat(message_repetitions);
        let message_3 = b"ayyc".repeat(message_repetitions);
        let message_4 = b"ayyd".repeat(message_repetitions);

        let signature_1 = scheme.sign(&private_key_1, &message_1);
        let signature_2 = scheme.sign(&private_key_2, &message_2);
        let signature_3 = scheme.sign(&private_key_3, &message_3);
        let signature_4 = scheme.sign(&private_key_4, &message_4);

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
        verify_aggregate(AugSchemeMPL::new(), 1);
    }

    #[test]
    fn verify_aggregate_basic() {
        verify_aggregate(BasicSchemeMPL::new(), 1);
    }

    #[test]
    fn verify_aggregate_legacy() {
        verify_aggregate(LegacySchemeMPL::new(), 8);
    }

    #[test]
    fn legacy_message_lengths() {
        let scheme = LegacySchemeMPL::new();
        let key1 = PrivateKey::key_gen(&scheme, &[1; 32]).unwrap();
        let key2 = PrivateKey::key_gen(&scheme, &[2; 32]).unwrap();
        let public1 = key1.g1_element().unwrap();
        let public2 = key2.g1_element().unwrap();
        let message1 = [0xAB; 64];
        let message2 = [0xCD; 64];
        let signature1 = scheme.try_sign(&key1, &message1[..32]).unwrap();
        let signature2 = scheme.try_sign(&key2, &message2[..32]).unwrap();
        let aggregate = scheme.aggregate_sigs([&signature1, &signature2]);

        for length in 0..32 {
            let short = &message1[..length];
            assert!(scheme.try_sign(&key1, short).is_err());
            assert!(std::panic::catch_unwind(|| scheme.sign(&key1, short)).is_err());
            assert!(!scheme.verify(&public1, short, &signature1));
            assert!(!scheme.aggregate_verify(
                [&public1, &public2],
                [short, &message2[..32]],
                &aggregate
            ));
            assert!(!scheme.aggregate_verify(
                [&public1, &public2],
                [&message1[..32], &message2[..length]],
                &aggregate
            ));

            // Both exported C signing paths must contain the native length exception.
            for result in unsafe {
                [
                    LegacySchemeMPLSign(
                        scheme.as_mut_ptr(),
                        key1.as_mut_ptr(),
                        message1.as_ptr() as *const _,
                        length,
                    ),
                    CoreMPLSign(
                        scheme.as_mut_ptr(),
                        key1.as_mut_ptr(),
                        message1.as_ptr() as *const _,
                        length,
                    ),
                ]
            } {
                if !result.is_null() {
                    unsafe { bls_dash_sys::G2ElementFree(result) };
                }
                assert!(result.is_null());
            }
        }

        for length in [32, 33, 64] {
            let message = &message1[..length];
            assert_eq!(scheme.try_sign(&key1, message).unwrap(), signature1);
            assert_eq!(scheme.sign(&key1, message), signature1);
            assert!(scheme.verify(&public1, message, &signature1));
            assert!(scheme.aggregate_verify(
                [&public1, &public2],
                [message, &message2[..length]],
                &aggregate
            ));
            let result = unsafe {
                CoreMPLSign(
                    scheme.as_mut_ptr(),
                    key1.as_mut_ptr(),
                    message1.as_ptr() as *const _,
                    length,
                )
            };
            assert!(!result.is_null());
            unsafe {
                assert!(bls_dash_sys::G2ElementIsEqual(result, signature1.c_element));
                bls_dash_sys::G2ElementFree(result);
            }
        }
    }
}
