// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//! evo-capture: the passive SOME/IP recorder for EntryNav2 / EntryEVO, ported from tools/evo_capture.cpp. RECEIVE
//! ONLY: one AF_PACKET socket that reads copies of the IPv4 traffic the head unit already sends and receives. Nothing
//! is ever sent, no interface flag is changed (no promiscuous mode), no BMW file is touched. Bounded in time (240 s by
//! default, 900 s at most) and size (the pcap closes at 24 MB, the summary goes on). A developer tool, not part of a
//! release: make rust builds it, make dev-tools RUST_TOOLS=evo-capture puts it in place of the C++ one.
//!   evo-capture --someip [seconds]     record, then summarize
//!   evo-capture --selftest             parser check on synthetic packets (PC/QEMU)

use std::io::{self, Write};
use std::process::ExitCode;

use evo_capture::{selftest, DEFAULT_SECONDS, MAX_SECONDS};

/// strtol(text, NULL, 10) as the C++ calls it: leading blanks, a sign, then digits up to the first other character.
/// Out of range saturates, as strtol does; either way the duration is refused.
fn strtol(text: &str) -> i64 {
    let t = text.trim_start_matches([' ', '\t', '\n', '\x0b', '\x0c', '\r']);
    let (negative, digits) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    let mut value: i64 = 0;
    for d in digits.bytes().take_while(u8::is_ascii_digit) {
        value = value.saturating_mul(10).saturating_add(i64::from(d - b'0'));
    }
    if negative {
        -value
    } else {
        value
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args_os().map(|a| a.to_string_lossy().into_owned()).collect();
    let first = args.get(1).map(String::as_str);
    if first == Some("--selftest") {
        let mut out = io::stdout().lock();
        let ok = selftest(&mut out).unwrap_or(false);
        let _ = out.flush();
        return ExitCode::from(if ok { 0 } else { 1 });
    }
    if first == Some("--someip") {
        let seconds = args.get(2).map_or(DEFAULT_SECONDS, |a| strtol(a));
        if !(10..=MAX_SECONDS).contains(&seconds) {
            eprintln!("REFUSED: duration 10..{MAX_SECONDS} s");
            return ExitCode::from(2);
        }
        return ExitCode::from(record::record(seconds));
    }
    println!(
        "Usage: evo-capture --someip [seconds] | --selftest\n\
         Passive SOME/IP recorder (read only, nothing sent, no promiscuous mode), 240 s by default, 900 s at most.\n\
         File: /tmp/entry-aa-releve/someip-*.pcap (raw IPv4, Wireshark) and a text summary."
    );
    ExitCode::from(if first == Some("--help") { 0 } else { 2 })
}

#[cfg(target_os = "linux")]
mod record {
    use std::collections::HashMap;
    use std::ffi::CStr;
    use std::fs::{DirBuilder, File, OpenOptions};
    use std::io::{self, BufWriter};
    use std::os::unix::fs::DirBuilderExt;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    use evo_capture::{PcapWriter, Summary, MAX_BYTES, OUT_DIR, SNAP};

    static STOP: AtomicBool = AtomicBool::new(false);

    extern "C" fn on_signal(_: libc::c_int) {
        STOP.store(true, Ordering::Relaxed);
    }

    fn strerror(code: i32) -> String {
        // SAFETY: strerror returns a pointer to a NUL-terminated string the C library owns.
        unsafe { CStr::from_ptr(libc::strerror(code)) }
            .to_string_lossy()
            .into_owned()
    }

    fn errno() -> i32 {
        io::Error::last_os_error().raw_os_error().unwrap_or(0)
    }

    // time_t is 32 bits in the head unit's musl and 64 on a PC, where the conversion changes nothing.
    #[allow(clippy::useless_conversion)]
    fn monotonic_seconds() -> i64 {
        let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
        // SAFETY: t is a valid timespec for the call to fill.
        unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut t) };
        i64::from(t.tv_sec)
    }

    fn be16(p: &[u8]) -> u32 {
        u32::from(p[0]) << 8 | u32::from(p[1])
    }

    /// "/someip-YYYYmmdd-HHMMSS.pcap" in local time, as strftime writes it for the C++.
    fn file_name() -> String {
        // SAFETY: time and localtime_r write only into the values given to them.
        let tm = unsafe {
            let now = libc::time(std::ptr::null_mut());
            let mut tm: libc::tm = std::mem::zeroed();
            libc::localtime_r(&now, &mut tm);
            tm
        };
        format!(
            "/someip-{:04}{:02}{:02}-{:02}{:02}{:02}.pcap",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec
        )
    }

    fn interface_name(index: i32) -> String {
        let mut name = [0 as libc::c_char; libc::IF_NAMESIZE + 1];
        // SAFETY: name holds IF_NAMESIZE bytes and a NUL, as if_indextoname needs.
        if unsafe { libc::if_indextoname(index as libc::c_uint, name.as_mut_ptr()) }.is_null() {
            return format!("if{index}");
        }
        // SAFETY: if_indextoname wrote a NUL-terminated name into the buffer.
        unsafe { CStr::from_ptr(name.as_ptr()) }.to_string_lossy().into_owned()
    }

    pub fn record(seconds: i64) -> u8 {
        // SAFETY: geteuid has no preconditions.
        if unsafe { libc::geteuid() } != 0 {
            eprintln!("REFUSED: root required (read-only AF_PACKET socket)");
            return 2;
        }
        if let Err(e) = DirBuilder::new().mode(0o700).create(OUT_DIR) {
            if e.kind() != io::ErrorKind::AlreadyExists {
                eprintln!("REFUSED: cannot create {OUT_DIR}");
                return 2;
            }
        }
        if !std::fs::symlink_metadata(OUT_DIR).is_ok_and(|m| m.is_dir()) {
            eprintln!("REFUSED: {OUT_DIR} invalid");
            return 2;
        }
        // ETH_P_ALL: also the head unit's outgoing copies.
        let protocol = i32::from((libc::ETH_P_ALL as u16).to_be());
        // SAFETY: a plain socket call; the descriptor is closed before every return below.
        let s = unsafe { libc::socket(libc::PF_PACKET, libc::SOCK_DGRAM | libc::SOCK_CLOEXEC, protocol) };
        if s < 0 {
            eprintln!("REFUSED: AF_PACKET socket: {}", strerror(errno()));
            return 2;
        }
        let rcv: libc::c_int = 4 * 1024 * 1024;
        // SAFETY: rcv outlives the call and its size is given.
        unsafe {
            libc::setsockopt(
                s,
                libc::SOL_SOCKET,
                libc::SO_RCVBUF,
                (&rcv as *const libc::c_int).cast(),
                std::mem::size_of::<libc::c_int>() as libc::socklen_t,
            )
        };
        let path = format!("{OUT_DIR}{}", file_name());
        let file = match OpenOptions::new().write(true).create_new(true).open(&path) {
            Ok(file) => file,
            Err(e) => {
                eprintln!("REFUSED: {path}: {}", strerror(e.raw_os_error().unwrap_or(0)));
                // SAFETY: s is the socket opened above.
                unsafe { libc::close(s) };
                return 2;
            }
        };
        let mut pcap: PcapWriter<BufWriter<File>> = match PcapWriter::new(BufWriter::new(file)) {
            Ok(pcap) => pcap,
            Err(e) => {
                eprintln!("REFUSED: {path}: {}", strerror(e.raw_os_error().unwrap_or(0)));
                // SAFETY: s is the socket opened above.
                unsafe { libc::close(s) };
                return 2;
            }
        };
        // SAFETY: on_signal only stores to an atomic, which is safe in a signal handler.
        unsafe {
            let handler = on_signal as extern "C" fn(libc::c_int) as libc::sighandler_t;
            libc::signal(libc::SIGINT, handler);
            libc::signal(libc::SIGTERM, handler);
            libc::signal(libc::SIGHUP, handler);
            libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        }
        println!(
            "Passive recording {seconds} s -> {path} (read only, nothing sent, at most {} MB)",
            MAX_BYTES >> 20
        );
        let mut sum = Summary::default();
        let mut names: HashMap<i32, String> = HashMap::new();
        let mut buf = vec![0u8; 65536];
        let t0 = monotonic_seconds();
        let (mut last_tick, mut kept, mut full) = (0i64, 0u64, false);
        while !STOP.load(Ordering::Relaxed) {
            let elapsed = monotonic_seconds() - t0;
            if elapsed >= seconds {
                break;
            }
            if elapsed / 30 != last_tick {
                last_tick = elapsed / 30;
                println!(
                    "  ... {elapsed} s, {kept} IPv4 packets kept, {} KB, {} SOME/IP messages",
                    pcap.bytes >> 10,
                    sum.someip
                );
            }
            let mut pfd = libc::pollfd {
                fd: s,
                events: libc::POLLIN,
                revents: 0,
            };
            // SAFETY: pfd is one valid pollfd.
            if unsafe { libc::poll(&mut pfd, 1, 500) } <= 0 {
                continue;
            }
            // SAFETY: sockaddr_ll is plain data; all zeroes is a valid value.
            let mut from: libc::sockaddr_ll = unsafe { std::mem::zeroed() };
            let mut from_len = std::mem::size_of::<libc::sockaddr_ll>() as libc::socklen_t;
            // SAFETY: buf and from are valid for the sizes given; MSG_TRUNC returns the packet's real size.
            let n = unsafe {
                libc::recvfrom(
                    s,
                    buf.as_mut_ptr().cast(),
                    buf.len(),
                    libc::MSG_TRUNC,
                    (&mut from as *mut libc::sockaddr_ll).cast(),
                    &mut from_len,
                )
            };
            if n <= 0 || from.sll_protocol != (libc::ETH_P_IP as u16).to_be() {
                continue;
            }
            let ifname = names
                .entry(from.sll_ifindex)
                .or_insert_with(|| interface_name(from.sll_ifindex));
            if ifname == "lo" {
                continue; // local control traffic (aa-entrynav, HMI)
            }
            let len = n as usize;
            let cap = len.min(buf.len());
            let ihl = usize::from(buf[0] & 15) * 4;
            if cap >= 40 && buf[9] == 6 && (be16(&buf[ihl..]) == 22 || be16(&buf[ihl + 2..]) == 22) {
                sum.skipped += 1;
                continue; // never record the SSH session itself
            }
            sum.add_ipv4(ifname, &buf[..cap]);
            if !full {
                let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
                let snap = cap.min(SNAP);
                if pcap.bytes + snap as i64 + 16 > MAX_BYTES {
                    full = true;
                    println!("  size limit reached: pcap closed, the summary goes on");
                } else {
                    // Write errors are ignored, as the C++'s fwrite results are: the summary still comes.
                    let _ = pcap.add(now.as_secs() as u32, now.subsec_micros(), &buf[..snap], len);
                    kept += 1;
                }
            }
        }
        let bytes = pcap.bytes;
        let _ = pcap.finish();
        // SAFETY: s is the socket opened above, closed once.
        unsafe { libc::close(s) };
        println!("Done: {kept} packets in {path} ({} KB)", bytes >> 10);
        let mut out = io::stdout().lock();
        let _ = sum.print(&mut out);
        let _ = io::Write::flush(&mut out);
        0
    }
}

#[cfg(not(target_os = "linux"))]
mod record {
    /// The recording needs Linux's AF_PACKET sockets: on the head unit, or on a Linux PC.
    pub fn record(_seconds: i64) -> u8 {
        eprintln!("REFUSED: the recording runs on Linux only (AF_PACKET)");
        2
    }
}

#[cfg(test)]
mod tests {
    use super::strtol;

    #[test]
    fn strtol_reads_like_the_c_library() {
        assert_eq!(strtol("  30s"), 30);
        assert_eq!(strtol("+900"), 900);
        assert_eq!(strtol("-5"), -5);
        assert_eq!(strtol("abc"), 0);
        assert_eq!(strtol("99999999999999999999999"), i64::MAX);
    }
}
