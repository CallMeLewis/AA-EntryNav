// Copyright (c) 2026 CallMeLewis
//! evo-ric's recording: the line written for each message and the filter of the watch, as tools/evo_ric.cpp writes
//! them. src/main.rs is the program around them; tests/golden.rs checks both against what the C++ recorded.

use std::collections::BTreeSet;
use std::io::Write;

use aa_dbus::{Message, Value, MESSAGE_ERROR, METHOD_CALL, METHOD_RETURN, SIGNAL};

pub const CAR_BUS: &str = "unix:path=/tmp/session_bus_socket";
pub const IFACE: &str = "BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.RICServiceGuidance";
/// The services that may answer RIC calls: the F-series drawing service and the two instances
/// /etc/CommonApiDBus/dbusAddresses.conf names.
pub const SERVERS: [&str; 3] = ["BMW.RueKoIC", "BMW.Kombi", "BMW.NBTEvo"];
pub const DEFAULT_OUT: &str = "/var/local/data/AA/ric";
pub const MAX_LINE: usize = 16 * 1024;
const MAX_ARRAY: usize = 64;
const MAX_BYTES: usize = 512;
const MAX_STRING: usize = 400;
const MAX_DYNAMIC: usize = 16;
pub const SYNC_EVERY_MS: u64 = 2000;
pub const HEARTBEAT_MS: u64 = 30000;

pub const USAGE: &str = "usage: evo-ric --names [--address A]\n       \
                         evo-ric --watch [minutes] [--out DIR] [--max-mb N] [--address A]\n       \
                         evo-ric --selftest\n";

pub const SELFTEST_PASS: &str =
    "PASS evo-ric: the RIC call and its answer recorded with every argument, other traffic \
                                 left out, the caller answered only by the service";

fn append_string(out: &mut Vec<u8>, s: &str) {
    out.push(b'"');
    let bytes = s.as_bytes();
    for &c in bytes.iter().take(MAX_STRING) {
        match c {
            b'"' | b'\\' => out.extend_from_slice(&[b'\\', c]),
            0..=0x1f => out.extend_from_slice(format!("\\x{c:02x}").as_bytes()),
            _ => out.push(c),
        }
    }
    if bytes.len() > MAX_STRING {
        out.extend_from_slice(b"...");
    }
    out.push(b'"');
}

/// One value, its type first for the scalars ("u:7"), recursively for the containers.
fn append_value(out: &mut Vec<u8>, value: &Value, depth: usize) {
    if out.len() > MAX_LINE || depth > 32 {
        out.extend_from_slice(b"...");
        return;
    }
    let mut put = |s: String| out.extend_from_slice(s.as_bytes());
    match value {
        Value::Byte(v) => put(format!("y:{v}")),
        Value::Boolean(v) => put(format!("b:{}", *v as u8)),
        Value::Int16(v) => put(format!("n:{v}")),
        Value::Uint16(v) => put(format!("q:{v}")),
        Value::Int32(v) => put(format!("i:{v}")),
        Value::Uint32(v) => put(format!("u:{v}")),
        Value::Int64(v) => put(format!("x:{v}")),
        Value::Uint64(v) => put(format!("t:{v}")),
        Value::Double(v) => put(format!("d:{}", evo_text::format_g(*v, 6))),
        Value::String(s) | Value::ObjectPath(s) | Value::Signature(s) => {
            out.extend_from_slice(match value {
                Value::String(_) => b"s:",
                Value::ObjectPath(_) => b"o:",
                _ => b"g:",
            });
            append_string(out, s);
        }
        Value::UnixFd(_) => out.extend_from_slice(b"h:fd"),
        Value::Array(element, items) if element == "y" => {
            let bytes = value.as_bytes().unwrap_or_default();
            put(format!("ay[{}]:", items.len()));
            for b in bytes.iter().take(MAX_BYTES) {
                out.extend_from_slice(format!("{b:02x}").as_bytes());
            }
            if bytes.len() > MAX_BYTES {
                out.extend_from_slice(b"...");
            }
        }
        Value::Array(_, items) => {
            out.push(b'[');
            for (n, item) in items.iter().enumerate() {
                if n == MAX_ARRAY {
                    out.extend_from_slice(b",...");
                    break;
                }
                if n > 0 {
                    out.push(b',');
                }
                append_value(out, item, depth + 1);
            }
            out.push(b']');
        }
        Value::Struct(fields) => {
            out.push(b'(');
            for (n, field) in fields.iter().enumerate() {
                if n > 0 {
                    out.push(b',');
                }
                append_value(out, field, depth + 1);
            }
            out.push(b')');
        }
        Value::DictEntry(key, item) => {
            out.push(b'{');
            append_value(out, key, depth + 1);
            out.push(b',');
            append_value(out, item, depth + 1);
            out.push(b'}');
        }
        Value::Variant(inner) => {
            out.extend_from_slice(format!("v<{}>", inner.signature()).as_bytes());
            append_value(out, inner, depth + 1);
        }
    }
}

fn type_name(kind: u8) -> &'static str {
    match kind {
        METHOD_CALL => "call",
        METHOD_RETURN => "return",
        MESSAGE_ERROR => "error",
        SIGNAL => "signal",
        _ => "?",
    }
}

/// One line: when, type, serials, who to whom, what, and every argument.
pub fn describe(m: &Message, elapsed_ms: u64) -> Vec<u8> {
    let mut line = format!(
        "{elapsed_ms} {} s={} rs={} {} -> {} {} {}.{} sig={} args=",
        type_name(m.kind),
        m.serial,
        m.reply_serial(),
        m.sender().unwrap_or("-"),
        m.destination().unwrap_or("-"),
        m.path().unwrap_or("-"),
        m.interface().unwrap_or("-"),
        m.member().or(m.error_name()).unwrap_or("-"),
        m.signature(),
    )
    .into_bytes();
    for (n, arg) in m.body.iter().enumerate() {
        if n > 0 {
            line.push(b' ');
        }
        append_value(&mut line, arg, 0);
        if line.len() > MAX_LINE {
            break;
        }
    }
    if line.len() > MAX_LINE {
        line.truncate(MAX_LINE);
        line.extend_from_slice(b"...");
    }
    line
}

/// The watch's state, and its filter on every message the bus delivers.
pub struct Watch<W: Write> {
    pub file: W,
    pub bytes: usize,
    pub max_bytes: usize,
    pub calls: u32,
    pub answers: u32,
    pub signals: u32,
    pub dropped: u32,
    pub full: bool,
    /// Callees whose answers are matched already.
    pub answering: BTreeSet<String>,
    /// Match rules to add without waiting, as the filter asks for them.
    pub rules: Vec<String>,
}

impl<W: Write> Watch<W> {
    pub fn new(file: W, max_bytes: usize) -> Watch<W> {
        Watch {
            file,
            bytes: 0,
            max_bytes,
            calls: 0,
            answers: 0,
            signals: 0,
            dropped: 0,
            full: false,
            answering: SERVERS.iter().map(|s| s.to_string()).collect(),
            rules: Vec::new(),
        }
    }

    fn match_answers_from(&mut self, sender: &str) {
        if self.answering.contains(sender) || self.answering.len() >= MAX_DYNAMIC + 3 {
            return;
        }
        self.answering.insert(sender.into());
        for kind in ["method_return", "error"] {
            self.rules
                .push(format!("eavesdrop=true,type='{kind}',sender='{sender}'"));
        }
    }

    fn write_line(&mut self, line: &[u8]) {
        if self.full {
            self.dropped += 1;
            return;
        }
        if self.bytes + line.len() + 1 > self.max_bytes {
            let _ = writeln!(
                self.file,
                "# size limit of {} bytes reached: nothing more is recorded",
                self.max_bytes
            );
            self.full = true;
            self.dropped += 1;
            return;
        }
        let _ = self.file.write_all(line);
        let _ = self.file.write_all(b"\n");
        self.bytes += line.len() + 1;
    }

    /// The filter: RIC calls and signals and the answers of the services, written; the rest left out.
    pub fn on_message(&mut self, m: &Message, elapsed_ms: u64) {
        if m.sender() == Some(aa_dbus::BUS_NAME) {
            return; // the bus answering our own AddMatch, and its NameAcquired
        }
        let ric = m.interface() == Some(IFACE);
        if m.kind == METHOD_CALL && ric {
            self.calls += 1;
            if let Some(callee) = m.destination() {
                let callee = callee.to_string();
                self.match_answers_from(&callee); // a callee by unique name: its answers too
            }
        } else if m.kind == SIGNAL && ric {
            self.signals += 1;
        } else if m.kind == METHOD_RETURN || m.kind == MESSAGE_ERROR {
            self.answers += 1;
        } else if !ric {
            return;
        }
        self.write_line(&describe(m, elapsed_ms));
    }
}
