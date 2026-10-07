use bls_dash_sys as sys;
use std::{ffi::CStr, sync::mpsc, thread};

fn last_error_message() -> String {
    unsafe { CStr::from_ptr(sys::GetLastErrorMsg()) }
        .to_string_lossy()
        .into_owned()
}

#[test]
fn error_message_thread_ownership() {
    let (first_written_tx, first_written_rx) = mpsc::channel();
    let (second_written_tx, second_written_rx) = mpsc::channel();

    let first = thread::spawn(move || {
        let invalid = [0u8];
        let mut did_err = false;
        let element = unsafe {
            sys::G1ElementFromBytes(
                invalid.as_ptr() as *const _,
                invalid.len(),
                false,
                &mut did_err,
            )
        };
        assert!(did_err);
        assert!(element.is_null());
        let before = last_error_message();
        assert_eq!(before, "G1Element::FromBytes: Invalid size");
        first_written_tx.send(()).unwrap();
        second_written_rx.recv().unwrap();
        assert_eq!(last_error_message(), before);

        let mut valid = [0u8; 48];
        valid[0] = 0xc0;
        let element = unsafe {
            sys::G1ElementFromBytes(valid.as_ptr() as *const _, valid.len(), false, &mut did_err)
        };
        assert!(!did_err);
        assert!(!element.is_null());
        assert!(unsafe { sys::G1ElementIsValid(element) });
        unsafe { sys::G1ElementFree(element) };

        let key = unsafe {
            sys::PrivateKeyFromBytes(
                invalid.as_ptr() as *const _,
                invalid.len(),
                false,
                &mut did_err,
            )
        };
        assert!(did_err);
        assert!(key.is_null());
        assert_eq!(last_error_message(), "PrivateKey::FromBytes: Invalid size");
        assert_eq!(before, "G1Element::FromBytes: Invalid size");
    });
    let second = thread::spawn(move || {
        first_written_rx.recv().unwrap();
        let invalid = [0u8];
        let mut did_err = false;
        let element = unsafe {
            sys::G2ElementFromBytes(
                invalid.as_ptr() as *const _,
                invalid.len(),
                false,
                &mut did_err,
            )
        };
        assert!(did_err);
        assert!(element.is_null());
        assert_eq!(last_error_message(), "G2Element::FromBytes: Invalid size");
        second_written_tx.send(()).unwrap();

        let mut valid = [0u8; 96];
        valid[0] = 0xc0;
        let element = unsafe {
            sys::G2ElementFromBytes(valid.as_ptr() as *const _, valid.len(), false, &mut did_err)
        };
        assert!(!did_err);
        assert!(!element.is_null());
        assert!(unsafe { sys::G2ElementIsValid(element) });
        unsafe { sys::G2ElementFree(element) };
    });

    first.join().unwrap();
    second.join().unwrap();
}
