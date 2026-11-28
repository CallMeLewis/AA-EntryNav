// Copyright (c) 2026 CallMeLewis
//! evo-check's output and its read-only Bluetooth codec, as tools/evo_check.cpp and the monitor mode of
//! platform/alps_spp.cpp give them. src/main.rs is the program around them; tests/golden.rs checks them against
//! what the C++ recorded (tests/test_check_golden.cpp).

use aa_dbus::Value;

pub mod alps;

pub const USAGE: &str = "evo-check --scene | --context | --bluetooth | --monitor [seconds 1..60] | --help\n\
evo-check --dbus-names | --dbus-introspect <destination> <path> | --dbus-watch <path> [seconds 1..3600]\n\
Read and watch only: no UUID, configuration, surface or TCP port.\n\
--scene: IVI scene and candidate layers; each command is bounded to 2 seconds.\n\
--context: video, MOST, audio and inputs, network, ports and firewall, read only.\n\
--bluetooth: the Alps API, then spp_data_interface of instances 0 and 1.\n\
--monitor: bounded watch (30 seconds by default), Ctrl-C to end it.\n\
--dbus-names: system bus names, with the pid and command line of their process.\n\
--dbus-introspect: XML description of one bus object.\n\
--dbus-watch: messages on one bus path (60 seconds by default), arguments decoded.\n";

/// The bytes of a C string: up to its first NUL, as printf's %s prints it.
pub fn c_str(bytes: &[u8]) -> &[u8] {
    &bytes[..bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len())]
}

fn indent(out: &mut Vec<u8>, width: usize) {
    out.extend(std::iter::repeat_n(b' ', width));
}

fn container(out: &mut Vec<u8>, code: u8, children: &[&Value], depth: usize) {
    out.extend_from_slice(&[code, b' ', b'{', b'\n']);
    if depth < 12 {
        dump_values(out, children, depth + 1);
    }
    indent(out, depth * 2 + 2);
    out.extend_from_slice(b"}\n");
}

/// --dbus-watch's dump of a message's arguments: one per line, containers opened and closed with braces.
pub fn dump(out: &mut Vec<u8>, values: &[Value], depth: usize) {
    let refs: Vec<&Value> = values.iter().collect();
    dump_values(out, &refs, depth);
}

fn dump_values(out: &mut Vec<u8>, values: &[&Value], depth: usize) {
    for value in values {
        indent(out, depth * 2 + 2);
        let mut line = |s: String| out.extend_from_slice(s.as_bytes());
        match value {
            Value::Array(element, items) if element == "y" => {
                let bytes = value.as_bytes().unwrap_or_default();
                let mut s = format!("ay[{}]", items.len());
                for b in bytes.iter().take(256) {
                    s += &format!(" {b:02x}");
                }
                s += if bytes.len() > 256 { " ...\n" } else { "\n" };
                line(s);
            }
            Value::Byte(v) => line(format!("y {v}\n")),
            Value::Boolean(v) => line(format!("b {}\n", *v as u8)),
            Value::Int16(v) => line(format!("n {v}\n")),
            Value::Uint16(v) => line(format!("q {v}\n")),
            Value::Int32(v) => line(format!("i {v}\n")),
            Value::Uint32(v) => line(format!("u {v}\n")),
            Value::Int64(v) => line(format!("x {v}\n")),
            Value::Uint64(v) => line(format!("t {v}\n")),
            Value::Double(v) => line(format!("d {}\n", evo_text::format_g(*v, 6))),
            // libdbus gives -1 for a descriptor the message does not carry; nothing here passes descriptors.
            Value::UnixFd(_) => line("h -1\n".into()),
            Value::String(s) | Value::ObjectPath(s) | Value::Signature(s) => {
                out.extend_from_slice(&[value.code(), b' ', b'"']);
                out.extend_from_slice(c_str(s.as_bytes()));
                out.extend_from_slice(b"\"\n");
            }
            Value::Array(_, items) => container(out, b'a', &items.iter().collect::<Vec<_>>(), depth),
            Value::Struct(fields) => container(out, b'r', &fields.iter().collect::<Vec<_>>(), depth),
            Value::DictEntry(key, item) => container(out, b'e', &[key, item], depth),
            Value::Variant(inner) => container(out, b'v', &[inner], depth),
        }
    }
}

/// The text between the first `<tag>` and the first `</tag>`, or "" when they are missing or in the wrong order.
fn xml_field<'a>(block: &'a [u8], tag: &str) -> &'a [u8] {
    let (start, end) = (format!("<{tag}>"), format!("</{tag}>"));
    match (find(block, start.as_bytes(), 0), find(block, end.as_bytes(), 0)) {
        (Some(a), Some(b)) if b >= a + start.len() => &block[a + start.len()..b],
        _ => b"",
    }
}

fn find(text: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if from > text.len() {
        return None;
    }
    text[from..]
        .windows(needle.len())
        .position(|w| w == needle)
        .map(|p| p + from)
}

fn hex_value(value: &[u8]) -> bool {
    (3..=18).contains(&value.len()) && value.starts_with(b"0x") && value[2..].iter().all(u8::is_ascii_hexdigit)
}

/// The static Alps configuration summary of --context, from the text of /etc/alps/evolution/evo_config.xml (None:
/// missing, unreadable, not a file or over 256 KiB).
pub fn alps_config_summary(text: Option<&[u8]>) -> Vec<u8> {
    let mut out = b"\n--- Static Alps configuration: supported services and SPP interface only ---\n".to_vec();
    let Some(text) = text else {
        out.extend_from_slice(b"[MISSING/UNREADABLE]\n");
        return out;
    };
    let mut pos = 0;
    let mut count = 0u32;
    while let Some(start) = find(text, b"<configuration>", pos) {
        let Some(end) = find(text, b"</configuration>", start) else {
            break;
        };
        let block = &text[start..end];
        pos = end + 16;
        let name = xml_field(block, "name");
        if name != b"\"supported_services\"" && name != b"\"spp_data_interface\"" {
            continue;
        }
        let (id, instance, value) = (
            xml_field(block, "id"),
            xml_field(block, "instance"),
            xml_field(block, "value"),
        );
        if !hex_value(id) || !hex_value(instance) || !hex_value(value) {
            out.extend_from_slice(b"[UNEXPECTED FORMAT]\n");
            continue;
        }
        for (i, part) in [name, b" id=", id, b" instance=", instance, b" value=", value]
            .iter()
            .enumerate()
        {
            out.extend_from_slice(if i % 2 == 0 { c_str(part) } else { part });
        }
        out.push(b'\n');
        count += 1;
    }
    out.extend_from_slice(
        format!("{count} static entries kept. This does not prove that an instance is free at run time.\n").as_bytes(),
    );
    out
}

/// What a command run by --scene or --context gave: its output, and how it ended (platform/evo_process.hpp).
#[derive(Clone, Debug, PartialEq)]
pub struct CommandReport {
    pub text: Vec<u8>,
    pub exit_code: i32,
    pub signal: i32,
    pub spawn_error: i32,
    pub timed_out: bool,
    pub truncated: bool,
}

impl Default for CommandReport {
    fn default() -> Self {
        CommandReport {
            text: Vec::new(),
            exit_code: -1,
            signal: 0,
            spawn_error: 0,
            timed_out: false,
            truncated: false,
        }
    }
}

/// The line that opens a command's report: "\n--- binary args ---\n".
pub fn command_header(binary: &str, args: &[&str]) -> Vec<u8> {
    let mut s = format!("\n--- {binary}");
    for a in args {
        s += &format!(" {a}");
    }
    s += " ---\n";
    s.into_bytes()
}

/// Whether the command counts as passed: it ran, and LayerManagerControl did not report a failure despite its zero
/// exit status. Then its output, kept even on failure, and the line saying how it ended.
pub fn command_report(ran: bool, report: &CommandReport) -> (bool, Vec<u8>) {
    let ok = ran && find(&report.text, b"LayerManagerService returned: failed", 0).is_none();
    let mut out = c_str(&report.text).to_vec();
    out.extend_from_slice(
        format!(
            "\n[{}] code={} signal={} spawn_error={} timed_out={} truncated={}\n",
            if ok { "OK" } else { "FAILED" },
            report.exit_code,
            report.signal,
            report.spawn_error,
            report.timed_out as u8,
            report.truncated as u8
        )
        .as_bytes(),
    );
    (ok, out)
}

/// strtoul of a whole argument in base 10, as --monitor reads its seconds: None when anything is left over, nothing
/// is read or the value overflows a `long` of `bits` bits.
pub fn whole_unsigned(text: &[u8], bits: u32) -> Option<u64> {
    let (value, read, overflow) = evo_text::strtoul(text, bits);
    (read != 0 && read == text.len() && !overflow).then_some(value)
}

/// strtol of a whole argument in base 10, as --dbus-watch reads its seconds.
pub fn whole_signed(text: &[u8], bits: u32) -> Option<i64> {
    let (value, read) = evo_text::strtol(text, bits);
    let max = (1i64 << (bits - 1)) - 1;
    (read != 0 && read == text.len() && value != max && value != -max - 1).then_some(value)
}
