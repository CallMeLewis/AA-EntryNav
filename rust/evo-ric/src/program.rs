// Copyright (c) 2026 CallMeLewis
//! The program around evo-ric's recording: the bus, the names, the watch loop, the self test and the arguments.

use std::fs::File;
use std::io::{Read, Write};
use std::os::unix::fs::DirBuilderExt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use aa_dbus::{Connection, Message, Received, Value, METHOD_CALL, METHOD_RETURN, NAME_FLAG_DO_NOT_QUEUE};
use evo_ric::{Watch, CAR_BUS, DEFAULT_OUT, HEARTBEAT_MS, IFACE, SELFTEST_PASS, SERVERS, SYNC_EVERY_MS, USAGE};

static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_signal(_: libc::c_int) {
    STOP.store(true, Ordering::SeqCst);
}

/// Milliseconds on the monotonic clock, as the C++'s monoMs (only differences are ever used).
fn mono_ms() -> u64 {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: a valid timespec to fill.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut t) };
    t.tv_sec as u64 * 1000 + t.tv_nsec as u64 / 1_000_000
}

/// At most 255 bytes of a small file, without its trailing line ends.
fn read_small(path: &str) -> Vec<u8> {
    let mut buf = [0u8; 255];
    let mut n = 0;
    if let Ok(mut f) = File::open(path) {
        // One fread of up to 255 bytes, as the C++: it may return fewer for /proc files only at their end.
        while n < buf.len() {
            match f.read(&mut buf[n..]) {
                Ok(0) | Err(_) => break,
                Ok(k) => n += k,
            }
        }
    }
    let mut s = buf[..n].to_vec();
    while matches!(s.last(), Some(b'\n') | Some(b'\r')) {
        s.pop();
    }
    s
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

fn uptime() -> String {
    let u = text(&read_small("/proc/uptime"));
    u.split(' ').next().unwrap_or("").to_string()
}

fn open_bus(address: &str) -> Option<Connection> {
    match Connection::open_bus(address) {
        Ok(c) => Some(c),
        Err(e) => {
            eprintln!("evo-ric: cannot open the bus at {address}: {}", e.message);
            None
        }
    }
}

/// The bus's well-known names with the process that owns each, one per line.
fn list_names(c: &mut Connection, out: &mut dyn Write) {
    let Ok(reply) = c.ask_bus("ListNames", None, 2000) else {
        let _ = writeln!(out, "# ListNames: no answer");
        return;
    };
    let mut names = Vec::new();
    if let Some(Value::Array(_, items)) = reply.body.first() {
        for item in items {
            match item {
                Value::String(s) if !s.starts_with(':') => names.push(s.clone()),
                Value::String(_) => {}
                _ => break,
            }
        }
    }
    let _ = writeln!(out, "# {} well-known names", names.len());
    for name in &names {
        let pid = match c.ask_bus("GetConnectionUnixProcessID", Some(name), 2000) {
            Ok(r) => match r.body.first() {
                Some(Value::Uint32(p)) => *p,
                _ => 0,
            },
            Err(_) => 0,
        };
        let comm = if pid != 0 {
            read_small(&format!("/proc/{pid}/comm"))
        } else {
            b"?".to_vec()
        };
        let _ = writeln!(out, "# name {name} pid {pid} {}", text(&comm));
    }
}

fn make_dirs(dir: &str) -> std::io::Result<()> {
    let bytes = dir.as_bytes();
    for i in 0..=bytes.len() {
        if i == bytes.len() || (bytes[i] == b'/' && i > 0) {
            let partial = &dir[..i];
            match std::fs::DirBuilder::new().mode(0o700).create(partial) {
                Err(e) if e.kind() != std::io::ErrorKind::AlreadyExists => return Err(e),
                _ => {}
            }
        }
    }
    Ok(())
}

fn errno_text(e: &std::io::Error) -> String {
    evo_text::strerror(e.raw_os_error().unwrap_or(0))
}

fn now_local(format: &str) -> String {
    // SAFETY: time, localtime_r and strftime with valid buffers.
    unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&now, &mut tm);
        let mut buf = [0u8; 64];
        let fmt = std::ffi::CString::new(format).expect("no NUL");
        let n = libc::strftime(buf.as_mut_ptr().cast(), buf.len(), fmt.as_ptr(), &tm);
        text(&buf[..n])
    }
}

fn flush_to_disk(f: &mut File) {
    let _ = f.flush();
    let _ = f.sync_all();
}

/// Records until `minutes` pass, a signal, or `stop` (the self test's). Returns the exit code and the log's path.
fn watch(address: &str, minutes: u32, dir: &str, max_bytes: usize, stop: Option<&AtomicBool>) -> (i32, String) {
    if let Err(e) = make_dirs(dir) {
        eprintln!("evo-ric: cannot create {dir}: {}", errno_text(&e));
        return (1, String::new());
    }
    let boot: String = text(&read_small("/proc/sys/kernel/random/boot_id"))
        .chars()
        .take(8)
        .collect();
    // SAFETY: time with no pointer.
    let started_at = unsafe { libc::time(std::ptr::null_mut()) };
    let mut name = format!(
        "{dir}/ric-{}-{started_at}.log",
        if boot.is_empty() { "noboot" } else { &boot }
    );
    while name.len() > 511 {
        name.pop(); // the C++'s 512-byte buffer
    }
    let file = match File::create(&name) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("evo-ric: cannot write {name}: {}", errno_text(&e));
            return (1, name);
        }
    };
    let t0 = mono_ms();
    let mut w = Watch::new(std::io::BufWriter::with_capacity(4096, file), max_bytes);
    let Some(mut conn) = open_bus(address) else {
        let _ = writeln!(w.file, "# cannot open the bus at {address}");
        let _ = w.file.flush();
        return (1, name);
    };
    let started = now_local("%Y-%m-%d %H:%M:%S");
    let _ = writeln!(
        w.file,
        "# evo-ric watch: {started}, boot {boot}, uptime {} s, bus {address}, {minutes} min at most, {max_bytes} bytes \
         at most",
        uptime()
    );
    let _ = writeln!(
        w.file,
        "# line: ms since start, type, serial, reply serial, sender -> destination, path, member, signature, arguments"
    );
    list_names(&mut conn, &mut w.file);
    // The interface's calls and signals, and the answers from the services that may serve it.
    let mut rules = vec![
        format!("eavesdrop=true,type='method_call',interface='{IFACE}'"),
        format!("eavesdrop=true,type='signal',interface='{IFACE}'"),
    ];
    for server in SERVERS {
        rules.push(format!("eavesdrop=true,type='method_return',sender='{server}'"));
        rules.push(format!("eavesdrop=true,type='error',sender='{server}'"));
    }
    for rule in &rules {
        let result = conn.add_match(rule);
        let _ = writeln!(
            w.file,
            "# match {rule}: {}",
            result.err().map(|e| e.message).as_deref().unwrap_or("accepted")
        );
    }
    flush_to_disk(w.file.get_mut());
    let until = t0 + minutes as u64 * 60000;
    let (mut synced_at, mut beat_at) = (mono_ms(), mono_ms());
    let stopped = || STOP.load(Ordering::SeqCst) || stop.is_some_and(|s| s.load(Ordering::SeqCst));
    while !stopped() && mono_ms() < until {
        match conn.receive(Duration::from_millis(500)) {
            Received::Message(m) => {
                w.on_message(&m, mono_ms() - t0);
                for rule in std::mem::take(&mut w.rules) {
                    let _ = conn.add_match_no_wait(&rule); // no wait for the answer, as inside the C++'s dispatch
                }
            }
            Received::Nothing => {}
            Received::Closed => {
                let _ = writeln!(w.file, "# the bus closed the connection");
                STOP.store(true, Ordering::SeqCst);
                let _ = writeln!(w.file, "# the bus connection was lost");
                break;
            }
        }
        let t = mono_ms();
        if t - beat_at >= HEARTBEAT_MS {
            let _ = writeln!(
                w.file,
                "# alive {} ms, uptime {} s, {} calls, {} answers, {} signals, {} bytes, {} dropped",
                t - t0,
                uptime(),
                w.calls,
                w.answers,
                w.signals,
                w.bytes,
                w.dropped
            );
            beat_at = t;
        }
        if t - synced_at >= SYNC_EVERY_MS {
            let _ = w.file.flush();
            let _ = w.file.get_ref().sync_all();
            synced_at = t;
        }
    }
    let _ = writeln!(
        w.file,
        "# end after {} ms: {} calls, {} answers, {} signals, {} bytes, {} dropped",
        mono_ms() - t0,
        w.calls,
        w.answers,
        w.signals,
        w.bytes,
        w.dropped
    );
    let _ = w.file.flush();
    let _ = w.file.get_ref().sync_all();
    println!(
        "evo-ric: {} calls, {} answers, {} signals recorded in {name}",
        w.calls, w.answers, w.signals
    );
    (0, name)
}

// ---- selftest (PC, under dbus-run-session) -------------------------------------------------------------------------

fn selftest() -> i32 {
    let Ok(address) = std::env::var("DBUS_SESSION_BUS_ADDRESS") else {
        eprintln!("FAIL evo-ric selftest: no session bus (run it under dbus-run-session)");
        return 1;
    };
    let mut template = *b"/tmp/evo-ric-test-XXXXXX\0";
    // SAFETY: a writable NUL-terminated template.
    if unsafe { libc::mkdtemp(template.as_mut_ptr().cast()) }.is_null() {
        return 1;
    }
    let dir = text(&template[..template.len() - 1]);
    // The service, as BMW's navigation runs it, and a caller.
    let (Some(mut server), Some(mut caller)) = (open_bus(&address), open_bus(&address)) else {
        return 1;
    };
    let _ = server.request_name("BMW.RueKoIC", NAME_FLAG_DO_NOT_QUEUE);
    let stop = Arc::new(AtomicBool::new(false));
    let watcher = {
        let (stop, address, dir) = (Arc::clone(&stop), address.clone(), dir.clone());
        std::thread::spawn(move || watch(&address, 1, &dir, 1 << 20, Some(&stop)))
    };
    std::thread::sleep(Duration::from_millis(500)); // its rules in place
                                                    // A RIC call with a struct and a variant, answered by the service; then a call on another interface.
    let call = Message::method_call(Some("BMW.RueKoIC"), "/BMW/RueKoIC", Some(IFACE), "ric_setResource").with([
        Value::Uint32(7),
        Value::Uint32(0x03000000),
        Value::Int32(3),
        Value::Struct(vec![
            Value::Byte(9),
            Value::Variant(Box::new(Value::String("Main St".into()))),
        ]),
    ]);
    let mut call = call;
    let serial = caller.send(&mut call).unwrap_or(0);
    let mut other = Message::method_call(
        Some(aa_dbus::BUS_NAME),
        aa_dbus::BUS_PATH,
        Some(aa_dbus::BUS_INTERFACE),
        "GetId",
    );
    let _ = caller.send(&mut other);
    let mut served = 0u32;
    let begin = Instant::now();
    while served == 0 && begin.elapsed() < Duration::from_secs(2) {
        if let Received::Message(m) = server.receive(Duration::from_millis(20)) {
            if m.kind == METHOD_CALL && m.interface() == Some(IFACE) {
                served += 1;
                let mut reply = Message::method_return(&m).with([Value::Int32(0)]);
                let _ = server.send(&mut reply);
            }
        }
    }
    let answered = caller.wait_reply(serial, 3000).is_ok_and(|r| r.kind == METHOD_RETURN);
    std::thread::sleep(Duration::from_millis(500));
    stop.store(true, Ordering::SeqCst);
    let (result, file) = watcher.join().unwrap_or((-1, String::new()));
    let log = std::fs::read(&file).map(|b| text(&b)).unwrap_or_default();
    let mut failures = 0;
    let expects: [(&str, bool); 7] = [
        ("call s=", true),
        (
            "BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.RICServiceGuidance.ric_setResource sig=uui(yv) args=u:7 \
             u:50331648 i:3 (y:9,v<s>s:\"Main St\")",
            true,
        ),
        ("return s=", true),
        ("sig=i args=i:0", true),
        ("GetId", false),
        ("# match eavesdrop=true,type='method_call'", true),
        ("# end after", true),
    ];
    for (text, present) in expects {
        if log.contains(text) != present {
            eprintln!(
                "FAIL evo-ric selftest: {} \"{text}\"",
                if present { "missing" } else { "unexpected" }
            );
            failures += 1;
        }
    }
    if result != 0 || !answered || served != 1 {
        eprintln!(
            "FAIL evo-ric selftest: watch {result}, answered {}, served {served}",
            answered as u8
        );
        failures += 1;
    }
    if failures > 0 {
        eprint!("{log}");
    }
    let _ = std::fs::remove_file(&file);
    let _ = std::fs::remove_dir(&dir);
    if failures > 0 {
        return 1;
    }
    println!("{SELFTEST_PASS}");
    0
}

/// strtol of the argument, clamped to lo..=hi, as the C++'s clampArg.
fn clamp_arg(text: Option<&[u8]>, default: u32, lo: u32, hi: u32) -> u32 {
    let Some(text) = text else {
        return default;
    };
    let (v, _) = evo_text::strtol(text, (std::mem::size_of::<libc::c_long>() * 8) as u32);
    v.clamp(lo as i64, hi as i64) as u32
}

fn usage() -> i32 {
    eprint!("{USAGE}");
    2
}

pub fn main() -> i32 {
    use std::os::unix::ffi::OsStrExt;
    let args: Vec<Vec<u8>> = std::env::args_os().map(|a| a.as_bytes().to_vec()).collect();
    if args.len() < 2 {
        return usage();
    }
    let mode = args[1].as_slice();
    if mode == b"--selftest" {
        return selftest();
    }
    if mode == b"--help" {
        print!("{USAGE}"); // also how QEMU checks that it starts
        return 0;
    }
    let mut address = std::env::var("DBUS_SESSION_BUS_ADDRESS").unwrap_or_else(|_| CAR_BUS.into());
    let mut dir = DEFAULT_OUT.to_string();
    let (mut minutes, mut max_mb) = (60u32, 8u32);
    let mut i = 2;
    while i < args.len() {
        let a = args[i].as_slice();
        if a == b"--address" && i + 1 < args.len() {
            i += 1;
            address = text(&args[i]);
        } else if a == b"--out" && i + 1 < args.len() {
            i += 1;
            dir = text(&args[i]);
        } else if a == b"--max-mb" && i + 1 < args.len() {
            i += 1;
            max_mb = clamp_arg(Some(&args[i]), 8, 1, 32);
        } else if mode == b"--watch" && a.first() != Some(&b'-') {
            minutes = clamp_arg(Some(a), 60, 1, 240);
        } else {
            return usage();
        }
        i += 1;
    }
    // SAFETY: plain C calls with no pointers but the handler, which only stores an atomic.
    unsafe {
        libc::setpriority(libc::PRIO_PROCESS, 0, 19); // the car's own programs first
        libc::signal(
            libc::SIGINT,
            on_signal as extern "C" fn(libc::c_int) as libc::sighandler_t,
        );
        libc::signal(
            libc::SIGTERM,
            on_signal as extern "C" fn(libc::c_int) as libc::sighandler_t,
        );
        libc::signal(libc::SIGHUP, libc::SIG_IGN); // the SSH session that started it may go
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    }
    if mode == b"--names" {
        let Some(mut c) = open_bus(&address) else {
            return 1;
        };
        let mut out = std::io::stdout().lock();
        let _ = writeln!(out, "# bus {address}");
        list_names(&mut c, &mut out);
        return 0;
    }
    if mode == b"--watch" {
        return watch(&address, minutes, &dir, (max_mb as usize) << 20, None).0;
    }
    usage()
}
