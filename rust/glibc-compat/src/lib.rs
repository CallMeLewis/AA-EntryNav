// Copyright (c) 2026 CallMeLewis
//! The functions Rust's std calls that the car's glibc 2.15 lacks, defined here so that a glibc Rust program links
//! with the build image's GCC 4.7 against the copy of the car's system (docs/rust-port.md, step 2).
//!
//! - `getauxval` came in glibc 2.16. std calls it when it gives a new thread its signal stack (`AT_MINSIGSTKSZ`).
//!   This one reads the same entry from /proc/self/auxv, and returns 0 with `errno` set to `ENOENT` when the entry
//!   is not there, as glibc's does. std then uses `SIGSTKSZ`.
//!
//! Every glibc Rust staticlib links this crate (`use glibc_compat as _;`), and the link fails without it. A program
//! built against a newer glibc would get two definitions, so the definitions exist only for the ARM glibc target.

/// The value of entry `kind` in an auxiliary vector: pairs of native words, ending with `AT_NULL` (0).
pub fn auxv_entry(auxv: &[u8], kind: usize, word: usize) -> Option<usize> {
    let read = |at: usize| -> Option<usize> {
        let bytes = auxv.get(at..at + word)?;
        Some(bytes.iter().rev().fold(0usize, |value, &b| (value << 8) | b as usize))
    };
    let mut at = 0;
    loop {
        let key = read(at)?;
        if key == 0 {
            return None;
        }
        let value = read(at + word)?;
        if key == kind {
            return Some(value);
        }
        at += 2 * word;
    }
}

#[cfg(all(target_os = "linux", target_env = "gnu", target_arch = "arm"))]
mod car {
    use std::ffi::{c_int, c_ulong};
    use std::io::Read;

    const ENOENT: c_int = 2;

    extern "C" {
        fn __errno_location() -> *mut c_int;
    }

    /// glibc 2.16's getauxval, from /proc/self/auxv.
    #[no_mangle]
    pub extern "C" fn getauxval(kind: c_ulong) -> c_ulong {
        let mut auxv = [0u8; 1024];
        let mut length = 0;
        if let Ok(mut file) = std::fs::File::open("/proc/self/auxv") {
            while length < auxv.len() {
                match file.read(&mut auxv[length..]) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => length += n,
                }
            }
        }
        match super::auxv_entry(&auxv[..length], kind as usize, std::mem::size_of::<c_ulong>()) {
            Some(value) => value as c_ulong,
            None => {
                // SAFETY: glibc's errno of this thread.
                unsafe { *__errno_location() = ENOENT };
                0
            }
        }
    }
}
