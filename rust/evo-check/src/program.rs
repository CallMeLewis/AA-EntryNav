// Copyright (c) 2026 CallMeLewis
//! evo-check's program: the modes of tools/evo_check.cpp, in its order and with its output.

use std::io::{Read, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::OpenOptionsExt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use aa_dbus::{BusError, Connection, Message, Received, Value, BUS_INTERFACE, BUS_NAME, BUS_PATH};
use evo_check::alps::{DATA_INTERFACE, PATH as ALPS_PATH};
use evo_check::{alps_config_summary, command_header, command_report, dump, whole_signed, whole_unsigned, USAGE};

use crate::link::{system_bus_address, valid_path, AlpsLink};
use crate::process::run_command;

static STOPPED: AtomicBool = AtomicBool::new(false);

extern "C" fn stop(_: libc::c_int) {
    STOPPED.store(true, Ordering::SeqCst);
}

extern "C" fn expired(_: libc::c_int) {
    const MSG: &[u8] = b"evo-check: time limit reached, closing the probe.\n";
    // SAFETY: async-signal-safe calls only. Diagnostic-only process: the system closes its D-Bus sockets.
    unsafe {
        libc::write(2, MSG.as_ptr().cast(), MSG.len());
        libc::_exit(124);
    }
}

fn stopped() -> bool {
    STOPPED.load(Ordering::SeqCst)
}

/// Writes to stdout at once, errors ignored (a closed pipe must not end the probe).
pub fn out(bytes: &[u8]) {
    let mut stdout = std::io::stdout().lock();
    let _ = stdout.write_all(bytes);
    let _ = stdout.flush();
}

fn line(text: &str) {
    out(format!("{text}\n").as_bytes());
}

fn usage() {
    out(USAGE.as_bytes());
}

/// One command, bounded to 2 seconds, its output kept even when it fails.
fn command(binary: &str, args: &[&str]) -> bool {
    if stopped() {
        return false;
    }
    out(&command_header(binary, args));
    let (ran, report) = run_command(binary, args, 2000);
    let (ok, text) = command_report(ran, &report);
    out(&text);
    ok
}

/// A regular file of at most `limit` bytes, read without blocking on a device.
fn read_text(path: &str, limit: usize) -> Option<Vec<u8>> {
    let mut file = std::fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .ok()?;
    if !file.metadata().ok()?.is_file() {
        return None;
    }
    let mut out = Vec::new();
    let mut buf = [0u8; 1024];
    loop {
        match file.read(&mut buf) {
            Ok(0) => return Some(out),
            Ok(n) => {
                out.extend_from_slice(&buf[..n]);
                if out.len() > limit {
                    return None;
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(_) => return None,
        }
    }
}

fn context() -> i32 {
    let busybox = "/bin/busybox.nosuid";
    command(busybox, &["uname", "-a"]);
    command(busybox, &["ls", "-l", "/dev/dri/card0", "/dev/cmem", "/dev/rpmsg-dce"]);
    command(
        busybox,
        &["ls", "-l", "/run/system/0/wayland-0", "/run/dbus/system_bus_socket"],
    );
    command(busybox, &["ls", "-l", "/dev/btspp0", "/dev/btspp1", "/dev/input"]);
    let paths = [
        "/etc/version",
        "/proc/uptime",
        "/proc/modules",
        "/sys/class/graphics/fb0/name",
        "/sys/class/graphics/fb0/virtual_size",
        "/sys/class/graphics/fb0/bits_per_pixel",
        "/sys/class/graphics/fb0/stride",
        "/sys/class/graphics/fb0/modes",
        "/sys/bus/rpmsg/devices/rpmsg0/name",
        "/proc/bus/input/devices",
        "/proc/asound/cards",
        "/proc/asound/pcm",
    ];
    for path in paths {
        if stopped() {
            break;
        }
        line(&format!("\n--- {path} ---"));
        match read_text(path, 16384) {
            Some(text) => {
                let mut shown = evo_check::c_str(&text).to_vec();
                shown.push(b'\n');
                out(&shown);
            }
            None => line("[MISSING/UNREADABLE/TOO LONG] Some entries are optional on this head unit."),
        }
    }
    if !stopped() {
        out(&alps_config_summary(
            read_text("/etc/alps/evolution/evo_config.xml", 262144).as_deref(),
        ));
    }
    // Only documented queries. Never register a MOST block or inject a message.
    command("/usr/local/bin/vmostctl", &["--list-fblock"]);
    command("/usr/local/bin/vmostctl", &["--apps"]);
    command(busybox, &["ifconfig", "wlan0"]);
    command(busybox, &["ip", "route", "show"]);
    command(busybox, &["netstat", "-tuln"]);
    // Avoid the HU's iptables shell wrapper (-w): invoke the binary directly without waiting on a lock.
    command("/usr/sbin/xtables-multi", &["iptables", "-S"]);
    command(busybox, &["df", "-h", "/tmp"]);
    line("\nContext collected. Look for FAILED and MISSING as well; no setting changed.");
    i32::from(stopped())
}

fn scene(ctl: &str) -> i32 {
    let mut ok = command(ctl, &["get", "scene"]);
    if !stopped() {
        ok = command(ctl, &["get", "screens"]) && ok;
    }
    if !stopped() {
        ok = command(ctl, &["get", "screen", "0"]) && ok;
    }
    if !stopped() {
        ok = command(ctl, &["get", "layers"]) && ok;
    }
    if !stopped() {
        ok = command(ctl, &["get", "surfaces"]) && ok;
    }
    command(ctl, &["get", "keyboard", "focus"]);
    for layer in ["0x100", "0x300", "0x2000", "0x3000"] {
        if stopped() {
            break;
        }
        command(ctl, &["get", "layer", layer]); // Candidate layers need not all exist.
    }
    for surface in ["0x101", "0x2001", "0x3001"] {
        if stopped() {
            break;
        }
        command(ctl, &["get", "surface", surface]);
    }
    line("\nReading done. No surface or layer created or changed.");
    i32::from(!ok || stopped())
}

fn bluetooth(link: &AlpsLink) -> i32 {
    let xml = link.introspect(ALPS_PATH);
    line(&format!("--- API Alps ---\n{xml}"));
    let mut ok = !xml.is_empty();
    for instance in 0..2u8 {
        if stopped() {
            break;
        }
        match link.read_configuration(instance, DATA_INTERFACE, 2000) {
            Some(value) if value.len() == 1 => line(&format!(
                "SPP instance={instance} configuration=0x6203 value={}",
                value[0]
            )),
            _ => {
                line(&format!("SPP instance={instance}: configuration read not confirmed"));
                ok = false;
            }
        }
    }
    line("No UUID registered, no configuration changed.");
    i32::from(!ok || stopped())
}

// ---- Read-only D-Bus survey (the HU has no dbus-send/dbus-monitor). Nothing is sent to the car network:
// --dbus-names                    well-known bus names with their owner PID and command line
// --dbus-introspect <dest> <path> introspection XML of one object
// --dbus-watch <path> [seconds]   every message on that object path (monitor mode, else signal match), args decoded

/// A call and its answer; an error, or no answer in 3 s, printed and None.
fn dbus_call(c: &mut Connection, m: Message) -> Option<Message> {
    let member = m.member().unwrap_or("").to_string();
    match c.call(m, 3000) {
        Ok(r) => Some(r),
        Err(e) => {
            line(&format!("  error {member}: {}", e.message));
            None
        }
    }
}

/// A bus name as libdbus accepts it: a unique name, or two or more dot-separated elements.
fn valid_bus_name(name: &str) -> bool {
    let unique = name.starts_with(':');
    let body = if unique { &name[1..] } else { name };
    let elements: Vec<&str> = body.split('.').collect();
    name.len() <= 255
        && elements.len() >= 2
        && elements.iter().all(|e| {
            !e.is_empty()
                && e.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
                && (unique || !e.as_bytes()[0].is_ascii_digit())
        })
}

fn bus_call(member: &str) -> Message {
    Message::method_call(Some(BUS_NAME), BUS_PATH, Some(BUS_INTERFACE), member)
}

fn cmdline(pid: u32) -> Vec<u8> {
    let mut buf = Vec::new();
    if pid != 0 {
        if let Ok(f) = std::fs::File::open(format!("/proc/{pid}/cmdline")) {
            let _ = f.take(159).read_to_end(&mut buf);
        }
    }
    // Every NUL but the last becomes a space, as the C++ joins the arguments.
    let last = buf.len().saturating_sub(1);
    for byte in &mut buf[..last] {
        if *byte == 0 {
            *byte = b' ';
        }
    }
    evo_check::c_str(&buf).to_vec()
}

fn dbus_tool(args: &[Vec<u8>]) -> i32 {
    let mode = args[1].as_slice();
    let list = args.len() == 2 && mode == b"--dbus-names";
    let introspect = args.len() == 4 && mode == b"--dbus-introspect";
    let watch = (args.len() == 3 || args.len() == 4) && mode == b"--dbus-watch";
    let mut secs = 60i64;
    if watch && args.len() == 4 {
        match whole_signed(&args[3], (std::mem::size_of::<libc::c_long>() * 8) as u32) {
            Some(s) if (1..=3600).contains(&s) => secs = s,
            _ => {
                usage();
                return 2;
            }
        }
    }
    if !list && !introspect && !watch {
        usage();
        return 2;
    }
    let mut c = match Connection::open_bus(&system_bus_address()) {
        Ok(c) => c,
        Err(e) => {
            line(&format!("system bus unavailable: {}", e.message));
            return 1;
        }
    };
    let text = |a: &[u8]| String::from_utf8_lossy(a).into_owned();
    if list {
        let Some(r) = dbus_call(&mut c, bus_call("ListNames")) else {
            return 1;
        };
        let Some(Value::Array(element, names)) = r.body.first() else {
            return 1;
        };
        if element != "s" {
            return 1;
        }
        for name in names.iter().filter_map(Value::as_str) {
            if name.starts_with(':') {
                continue;
            }
            let mut pid = 0u32;
            if let Some(pr) = dbus_call(
                &mut c,
                bus_call("GetConnectionUnixProcessID").with([Value::String(name.into())]),
            ) {
                if let Some(Value::Uint32(p)) = pr.body.first() {
                    pid = *p;
                }
            }
            let mut shown = format!("{name}  pid {pid}  ").into_bytes();
            shown.extend_from_slice(&cmdline(pid));
            shown.push(b'\n');
            out(&shown);
        }
    } else if introspect {
        let (dest, path) = (text(&args[2]), text(&args[3]));
        if !valid_bus_name(&dest) || !valid_path(&path) {
            // libdbus refuses to build the call: a warning on stderr, nothing sent, nothing printed.
            eprintln!("evo-check: {dest} {path}: not a bus name and an object path");
        } else {
            let call = Message::method_call(
                Some(&dest),
                &path,
                Some("org.freedesktop.DBus.Introspectable"),
                "Introspect",
            );
            if let Some(Value::String(xml)) = dbus_call(&mut c, call).as_ref().and_then(|r| r.body.first()) {
                line(xml);
            }
        }
    } else {
        let path = text(&args[2]);
        let mut rule = format!("path='{path}'").into_bytes();
        rule.truncate(299); // the C++'s 300-byte buffer
        let rule = text(&rule);
        // dbus >= 1.9.10: monitor mode sees method calls and replies too.
        let monitoring = Message::method_call(
            Some(BUS_NAME),
            BUS_PATH,
            Some("org.freedesktop.DBus.Monitoring"),
            "BecomeMonitor",
        )
        .with([
            Value::Array("s".into(), vec![Value::String(rule.clone())]),
            Value::Uint32(0),
        ]);
        let monitor = c.call(monitoring, 3000).is_ok();
        if !monitor {
            let mut eavesdrop = format!("eavesdrop='true',{rule}").into_bytes();
            eavesdrop.truncate(339);
            let refused = c
                .add_match(&text(&eavesdrop))
                .err()
                .and_then(|_| c.add_match(&rule).err());
            if let Some(BusError { message, .. }) = refused {
                line(&format!("subscription refused: {message}"));
                return 1;
            }
        }
        line(&format!(
            "watching {path} for {secs} s ({})",
            if monitor { "monitor mode" } else { "subscription" }
        ));
        let end = now() + secs;
        while now() < end {
            match c.receive(Duration::from_millis(200)) {
                Received::Message(m) => print_message(&m),
                Received::Nothing => {}
                Received::Closed => break,
            }
            while c.has_queued() {
                if let Received::Message(m) = c.receive(Duration::ZERO) {
                    print_message(&m);
                }
            }
        }
    }
    0
}

fn now() -> i64 {
    // SAFETY: time with no pointer.
    unsafe { libc::time(std::ptr::null_mut()) as i64 }
}

fn print_message(m: &Message) {
    const TYPES: [&str; 5] = ["?", "call", "return", "error", "signal"];
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: a valid timespec to fill.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut t) };
    let mut text = format!(
        "[{}.{:03}] {} {} -> {} {} {}.{}\n",
        t.tv_sec,
        t.tv_nsec / 1_000_000,
        TYPES.get(m.kind as usize).unwrap_or(&"?"),
        m.sender().unwrap_or("-"),
        m.destination().unwrap_or("*"),
        m.path().unwrap_or("-"),
        m.interface().unwrap_or("-"),
        m.member().unwrap_or("-")
    )
    .into_bytes();
    dump(&mut text, &m.body, 0);
    out(&text);
}

pub fn main() -> i32 {
    let args: Vec<Vec<u8>> = std::env::args_os().map(|a| a.as_bytes().to_vec()).collect();
    let argc = args.len();
    let is = |i: usize, s: &str| args.get(i).is_some_and(|a| a == s.as_bytes());
    if argc >= 2 && args[1].starts_with(b"--dbus-") {
        return dbus_tool(&args);
    }
    if argc == 2 && is(1, "--help") {
        usage();
        return 0;
    }
    let scene_mode = (argc == 2 || argc == 3) && is(1, "--scene"); // optional: another CLI (host tests)
    let surroundings = argc == 2 && is(1, "--context");
    let bt_mode = argc == 2 && is(1, "--bluetooth");
    let monitor = (argc == 2 || argc == 3) && is(1, "--monitor");
    if !scene_mode && !surroundings && !bt_mode && !monitor {
        usage();
        return 2;
    }
    let mut seconds = 30u32;
    if monitor && argc == 3 {
        match whole_unsigned(&args[2], (std::mem::size_of::<libc::c_ulong>() * 8) as u32) {
            Some(v) if (1..=60).contains(&v) => seconds = v as u32,
            _ => return 2,
        }
    }
    // SAFETY: handlers that store an atomic, or write and exit; plain C calls otherwise.
    unsafe {
        let stop_handler = stop as extern "C" fn(libc::c_int) as libc::sighandler_t;
        libc::signal(libc::SIGINT, stop_handler);
        libc::signal(libc::SIGTERM, stop_handler);
        libc::signal(libc::SIGHUP, stop_handler);
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        libc::signal(
            libc::SIGALRM,
            expired as extern "C" fn(libc::c_int) as libc::sighandler_t,
        );
        libc::alarm(if monitor { seconds + 10 } else { 30 });
    }
    if std::env::var_os("XDG_RUNTIME_DIR").is_none() {
        std::env::set_var("XDG_RUNTIME_DIR", "/run/system/0");
    }
    if surroundings {
        return context();
    }
    if scene_mode {
        let ctl = if argc == 3 {
            String::from_utf8_lossy(&args[2]).into_owned()
        } else {
            "/usr/bin/LayerManagerControl".into()
        };
        return scene(&ctl);
    }
    let Some(mut link) = AlpsLink::start(0) else {
        return 1;
    };
    if bt_mode {
        return bluetooth(&link);
    }
    line(&format!(
        "Watching Bluetooth for {seconds} seconds. Nothing sent over SPP."
    ));
    let begin = Instant::now();
    while !stopped() && link.running() && begin.elapsed() < Duration::from_millis(seconds as u64 * 1000) {
        std::thread::sleep(Duration::from_millis(100));
    }
    let alive = link.running();
    link.stop();
    line("Watch over; D-Bus connection closed.");
    i32::from(!alive)
}
