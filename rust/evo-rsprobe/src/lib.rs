// Copyright (c) 2026 CallMeLewis
//! evo-rsprobe: checks that a glibc-linked Rust program works on the head unit (docs/rust-port.md, step 2).
//!
//! The program is this staticlib, built for `armv7-unknown-linux-gnueabihf` and linked by the build image's GCC 4.7
//! with `main.c` against the copy of the car's system. It prints one line per check, each ending in `ok` when the
//! check passes, then `PASS evo-rsprobe: N checks` (exit 0) or `FAIL evo-rsprobe: ...` (exit 1). It changes nothing
//! on the car: it writes and removes one folder under /tmp and talks to itself on the loopback interface.
//!
//! The std checks cover what the service will use: threads, thread-locals, locks, both clocks, the random keys of
//! `HashMap` (`getrandom`, with std's fallback to /dev/urandom on a kernel without it), files and their metadata
//! (`statx`, with the fallback to `stat64`), UDP, TCP and Unix sockets, /proc, and starting a program. The car
//! library checks load each library the service uses with `dlopen` and look up the functions it calls, and call
//! the car's libssl through a link, as the TLS session will.

// getauxval and the other functions the car's glibc 2.15 lacks.
use glibc_compat as _;

use std::collections::hash_map::RandomState;
use std::hash::BuildHasher;
use std::io::{Read, Write};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// One check: its name and what it found, or why it failed.
pub struct Outcome {
    pub name: &'static str,
    pub result: Result<String, String>,
}

/// The line printed for one check.
pub fn line(outcome: &Outcome) -> String {
    match &outcome.result {
        Ok(detail) => format!("{}: {detail}, ok", outcome.name),
        Err(why) => format!("{}: FAILED, {why}", outcome.name),
    }
}

/// The last line, and whether every check passed.
pub fn verdict(outcomes: &[Outcome]) -> (String, bool) {
    let failed: Vec<&str> = outcomes.iter().filter(|o| o.result.is_err()).map(|o| o.name).collect();
    if failed.is_empty() {
        (format!("PASS evo-rsprobe: {} checks", outcomes.len()), true)
    } else {
        let names = failed.join(", ");
        (
            format!(
                "FAIL evo-rsprobe: {} of {} checks failed: {names}",
                failed.len(),
                outcomes.len()
            ),
            false,
        )
    }
}

fn check(name: &'static str, run: impl FnOnce() -> Result<String, String>) -> Outcome {
    Outcome { name, result: run() }
}

fn text<E: std::fmt::Display>(error: E) -> String {
    error.to_string()
}

/// The checks of std, which run on any system with threads and a file system.
pub fn std_checks() -> Vec<Outcome> {
    let mut out = vec![
        check("thread", || {
            let answer = std::thread::spawn(|| 40 + 2)
                .join()
                .map_err(|_| "the thread did not end".to_string())?;
            let named = std::thread::Builder::new()
                .name("probe".into())
                .spawn(|| std::thread::current().name().map(str::to_owned))
                .map_err(text)?
                .join()
                .map_err(|_| "the named thread did not end".to_string())?;
            match (answer, named.as_deref()) {
                (42, Some("probe")) => Ok("spawn, join and a named thread".into()),
                other => Err(format!("got {other:?}")),
            }
        }),
        check("thread-local", || {
            thread_local!(static VALUE: std::cell::Cell<u32> = const { std::cell::Cell::new(7) });
            VALUE.with(|v| v.set(8));
            let other = std::thread::spawn(|| VALUE.with(|v| v.get()))
                .join()
                .map_err(|_| "no join".to_string())?;
            match (VALUE.with(|v| v.get()), other) {
                (8, 7) => Ok("one value per thread".into()),
                got => Err(format!("got {got:?}")),
            }
        }),
        check("lock", || {
            let pair = Arc::new((Mutex::new(false), Condvar::new()));
            let theirs = Arc::clone(&pair);
            let waker = std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(20));
                *theirs.0.lock().unwrap() = true;
                theirs.1.notify_all();
            });
            let mut ready = pair.0.lock().map_err(text)?;
            let begin = Instant::now();
            while !*ready && begin.elapsed() < Duration::from_secs(5) {
                ready = pair.1.wait_timeout(ready, Duration::from_secs(1)).map_err(text)?.0;
            }
            let woken = *ready;
            drop(ready);
            waker.join().map_err(|_| "no join".to_string())?;
            if woken {
                Ok("Mutex and Condvar woken by another thread".into())
            } else {
                Err("never woken".into())
            }
        }),
        check("monotonic clock", || {
            let begin = Instant::now();
            std::thread::sleep(Duration::from_millis(20));
            let slept = begin.elapsed();
            if (Duration::from_millis(20)..Duration::from_secs(2)).contains(&slept) {
                Ok(format!("a 20 ms sleep took {} ms", slept.as_millis()))
            } else {
                Err(format!("a 20 ms sleep took {slept:?}"))
            }
        }),
        check("system clock", || {
            let now = SystemTime::now().duration_since(UNIX_EPOCH).map_err(text)?.as_secs();
            if now > 1_600_000_000 {
                Ok(format!("{now} s since 1970"))
            } else {
                Err(format!("{now} s since 1970: the clock is not set"))
            }
        }),
        check("random", || {
            // RandomState takes its keys from getrandom, or /dev/urandom where the kernel lacks it.
            let (a, b) = (RandomState::new(), RandomState::new());
            let mut map = std::collections::HashMap::new();
            map.insert("key", 1);
            if a.hash_one(1u32) != b.hash_one(1u32) && map.get("key") == Some(&1) {
                Ok("HashMap keys drawn".into())
            } else {
                Err("two RandomStates hash alike".into())
            }
        }),
        check("files", || {
            let dir = std::env::temp_dir().join(format!("evo-rsprobe-{}", std::process::id()));
            let result = files(&dir);
            let _ = std::fs::remove_dir_all(&dir);
            result
        }),
        check("udp", || {
            let a = std::net::UdpSocket::bind("127.0.0.1:0").map_err(text)?;
            let b = std::net::UdpSocket::bind("127.0.0.1:0").map_err(text)?;
            b.set_read_timeout(Some(Duration::from_secs(2))).map_err(text)?;
            a.send_to(b"probe", b.local_addr().map_err(text)?).map_err(text)?;
            let mut buf = [0u8; 16];
            let (n, from) = b.recv_from(&mut buf).map_err(text)?;
            if &buf[..n] == b"probe" && from == a.local_addr().map_err(text)? {
                Ok("a datagram over the loopback".into())
            } else {
                Err(format!("received {:?} from {from}", &buf[..n]))
            }
        }),
        check("tcp", || {
            let listener = std::net::TcpListener::bind("127.0.0.1:0").map_err(text)?;
            let address = listener.local_addr().map_err(text)?;
            let server = std::thread::spawn(move || -> std::io::Result<()> {
                let (mut stream, _) = listener.accept()?;
                let mut buf = [0u8; 5];
                stream.read_exact(&mut buf)?;
                stream.write_all(&buf)
            });
            let mut client = std::net::TcpStream::connect_timeout(&address, Duration::from_secs(2)).map_err(text)?;
            client.set_read_timeout(Some(Duration::from_secs(2))).map_err(text)?;
            client.write_all(b"probe").map_err(text)?;
            let mut echo = [0u8; 5];
            client.read_exact(&mut echo).map_err(text)?;
            server.join().map_err(|_| "no join".to_string())?.map_err(text)?;
            if &echo == b"probe" {
                Ok("a connection and its echo".into())
            } else {
                Err(format!("echo {echo:?}"))
            }
        }),
    ];
    out.extend(unix_checks());
    out
}

fn files(dir: &std::path::Path) -> Result<String, String> {
    std::fs::create_dir_all(dir).map_err(text)?;
    let first = dir.join("a");
    std::fs::write(&first, b"probe\n").map_err(text)?;
    let second = dir.join("b");
    std::fs::rename(&first, &second).map_err(text)?;
    let read = std::fs::read(&second).map_err(text)?;
    // metadata() tries statx first and falls back to stat64 on a kernel without it.
    let length = std::fs::metadata(&second).map_err(text)?.len();
    let listed = std::fs::read_dir(dir).map_err(text)?.count();
    if read == b"probe\n" && length == 6 && listed == 1 {
        Ok("write, rename, read, metadata and a folder listing".into())
    } else {
        Err(format!("read {read:?}, length {length}, {listed} entries"))
    }
}

#[cfg(unix)]
fn unix_checks() -> Vec<Outcome> {
    use std::os::unix::net::{UnixDatagram, UnixStream};
    use std::os::unix::process::ExitStatusExt;
    vec![
        check("unix sockets", || {
            let (mut a, mut b) = UnixStream::pair().map_err(text)?;
            a.write_all(b"probe").map_err(text)?;
            let mut buf = [0u8; 5];
            b.read_exact(&mut buf).map_err(text)?;
            let (c, d) = UnixDatagram::pair().map_err(text)?;
            c.send(b"x").map_err(text)?;
            let mut one = [0u8; 1];
            d.recv(&mut one).map_err(text)?;
            if &buf == b"probe" && one == *b"x" {
                Ok("a stream pair and a datagram pair".into())
            } else {
                Err("other bytes came back".into())
            }
        }),
        check("proc", || {
            let maps = std::fs::read_to_string("/proc/self/maps").map_err(text)?;
            let uptime = std::fs::read_to_string("/proc/uptime").map_err(text)?;
            if maps.lines().count() > 3 && uptime.contains('.') {
                Ok(format!("{} mappings", maps.lines().count()))
            } else {
                Err("/proc/self/maps or /proc/uptime unlike Linux's".into())
            }
        }),
        check("spawn", || {
            let output = std::process::Command::new("/bin/sh")
                .args(["-c", "echo probe; exit 3"])
                .output()
                .map_err(text)?;
            if output.stdout == b"probe\n" && output.status.code() == Some(3) && output.status.signal().is_none() {
                Ok("/bin/sh ran, its output and exit code read".into())
            } else {
                Err(format!(
                    "{:?}, {:?}",
                    output.status,
                    String::from_utf8_lossy(&output.stdout)
                ))
            }
        }),
    ]
}

#[cfg(not(unix))]
fn unix_checks() -> Vec<Outcome> {
    Vec::new()
}

/// The car's libraries: each one the service uses, with the functions it looks up.
pub const CAR_LIBRARIES: &[(&str, &[&str])] = &[
    ("libxsvideo.so.4", &["xsv_open", "xsv_close", "xsv_configure_stream"]),
    ("libdrm_omap.so.1", &["omap_device_new", "omap_bo_new"]),
    ("libdrm.so.2", &["drmIoctl"]),
    (
        "libssl.so.1.0.0",
        &["SSL_library_init", "TLSv1_2_client_method", "SSL_CTX_new"],
    ),
    (
        "libcrypto.so.1.0.0",
        &["ERR_peek_last_error", "PEM_read_bio_PrivateKey"],
    ),
    (
        "libdbus-1.so.3",
        &["dbus_bus_get_private", "dbus_connection_open_private"],
    ),
    (
        "libvmostclient.so.1",
        &[
            "vmostConnect",
            "vmostFblockShadowRegister",
            "vmostFblockShadowDeregister",
            "vmostFileDescriptorGet",
            "vmostProcessMessages",
            "vmostDisconnect",
            "vmostMostMsgSend",
        ],
    ),
    (
        "libasound.so.2",
        &["snd_pcm_open", "snd_pcm_writei", "snd_pcm_readi", "snd_pcm_set_params"],
    ),
    (
        "libwayland-client.so.0",
        &["wl_display_connect", "wl_proxy_marshal", "wl_proxy_add_listener"],
    ),
    (
        "libivi-application.so.0",
        &["ivi_application_interface", "ivi_surface_interface"],
    ),
];

#[cfg(all(target_os = "linux", target_env = "gnu", target_arch = "arm"))]
mod car {
    use super::{check, Outcome, CAR_LIBRARIES};
    use std::ffi::{c_char, c_int, c_long, c_void, CStr, CString};

    const RTLD_NOW: c_int = 2;

    extern "C" {
        fn dlopen(file: *const c_char, flags: c_int) -> *mut c_void;
        fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
        fn dlerror() -> *const c_char;
    }

    // The car's libssl 1.0.0 and libcrypto, linked (main.c's link adds -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0).
    extern "C" {
        fn SSL_library_init() -> c_int;
        fn TLSv1_2_client_method() -> *const c_void;
        fn SSL_CTX_new(method: *const c_void) -> *mut c_void;
        fn SSL_CTX_set_cipher_list(ctx: *mut c_void, list: *const c_char) -> c_int;
        fn SSL_CTX_free(ctx: *mut c_void);
        fn SSLeay_version(kind: c_int) -> *const c_char;
        fn SSLeay() -> c_long;
    }

    fn last_error() -> String {
        // SAFETY: dlerror returns null or a string that stays valid until the next dl* call on this thread.
        let why = unsafe { dlerror() };
        if why.is_null() {
            "no reason given".into()
        } else {
            unsafe { CStr::from_ptr(why) }.to_string_lossy().into_owned()
        }
    }

    extern "C" {
        // glibc-compat's, since the car's glibc 2.15 has none.
        fn getauxval(kind: std::ffi::c_ulong) -> std::ffi::c_ulong;
    }

    pub fn checks() -> Vec<Outcome> {
        let mut out = vec![check("getauxval", || {
            const AT_PAGESZ: std::ffi::c_ulong = 6;
            const AT_HWCAP: std::ffi::c_ulong = 16;
            // SAFETY: plain C calls with no pointers.
            let (page, hwcap) = unsafe { (getauxval(AT_PAGESZ), getauxval(AT_HWCAP)) };
            if page == 4096 && hwcap != 0 {
                Ok(format!(
                    "glibc-compat's: page size {page}, hardware capabilities {hwcap:#x}"
                ))
            } else {
                Err(format!("page size {page}, hardware capabilities {hwcap:#x}"))
            }
        })];
        out.extend(CAR_LIBRARIES.iter().map(|&(library, symbols)| Outcome {
            name: library,
            result: load(library, symbols),
        }));
        out.push(check("linked libssl", || {
            // SAFETY: plain C calls into OpenSSL 1.0.0 with the arguments its headers give; the context is freed.
            unsafe {
                SSL_library_init();
                let ctx = SSL_CTX_new(TLSv1_2_client_method());
                if ctx.is_null() {
                    return Err("SSL_CTX_new(TLSv1_2_client_method()) gave nothing".into());
                }
                let ciphers = SSL_CTX_set_cipher_list(ctx, c"AES128-SHA".as_ptr());
                SSL_CTX_free(ctx);
                let version = CStr::from_ptr(SSLeay_version(0)).to_string_lossy().into_owned();
                if ciphers != 1 {
                    return Err(format!("{version} refused the cipher list"));
                }
                Ok(format!(
                    "{version} ({:#x}), a TLS 1.2 client context made and freed",
                    SSLeay()
                ))
            }
        }));
        out
    }

    fn load(library: &str, symbols: &[&str]) -> Result<String, String> {
        let name = CString::new(library).map_err(|e| e.to_string())?;
        // SAFETY: dlopen with a valid C string; the library stays loaded until the program ends.
        let handle = unsafe { dlopen(name.as_ptr(), RTLD_NOW) };
        if handle.is_null() {
            return Err(format!("dlopen: {}", last_error()));
        }
        let missing: Vec<&str> = symbols
            .iter()
            .copied()
            .filter(|symbol| {
                let symbol = CString::new(*symbol).expect("no NUL in a symbol name");
                // SAFETY: a valid handle and C string; the result is only compared with null.
                unsafe { dlsym(handle, symbol.as_ptr()) }.is_null()
            })
            .collect();
        if missing.is_empty() {
            Ok(format!("loaded, {} functions found", symbols.len()))
        } else {
            Err(format!("loaded, but {} missing", missing.join(", ")))
        }
    }
}

/// Every check, the car's libraries included in the program for the head unit (glibc on ARM), whose link brings
/// in the car's libssl.
pub fn all_checks() -> Vec<Outcome> {
    let out = std_checks();
    #[cfg(all(target_os = "linux", target_env = "gnu", target_arch = "arm"))]
    let out = out.into_iter().chain(car::checks()).collect();
    out
}

/// The program, called by main.c. Prints each check and the verdict; 0 when every check passed.
#[no_mangle]
pub extern "C" fn evo_rsprobe_main() -> std::ffi::c_int {
    let outcomes = all_checks();
    let mut stdout = std::io::stdout().lock();
    for outcome in &outcomes {
        let _ = writeln!(stdout, "{}", line(outcome));
    }
    let (last, passed) = verdict(&outcomes);
    let _ = writeln!(stdout, "{last}");
    let _ = stdout.flush();
    if passed {
        0
    } else {
        1
    }
}
