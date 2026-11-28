// Copyright (c) 2026 CallMeLewis
// evo-check against what the C++ tools/evo_check.cpp and platform/alps_spp.cpp recorded (tests/test_check_golden.cpp):
// --dbus-watch's dump of the D-Bus corpus, the Alps configuration summary of each case, the usage text, the bytes
// of every request the Alps link sends, and its log of the signals the fake Alps service sent.

use aa_dbus::Message;
use evo_check::alps::{introspect_request, read_configuration_request, AlpsMonitor, DATA_INTERFACE, PATH};
use evo_check::{alps_config_summary, dump, USAGE};

// Compiled in, so that the test built for ARM runs anywhere (under QEMU, tests/run_qemu.py).
const ALPS_CASES: [&[u8]; 4] = [
    include_bytes!("../../../tests/fixtures/evo_check/alps-config-1.xml"),
    include_bytes!("../../../tests/fixtures/evo_check/alps-config-2.xml"),
    include_bytes!("../../../tests/fixtures/evo_check/alps-config-3.xml"),
    include_bytes!("../../../tests/fixtures/evo_check/alps-config-4.xml"),
];
const MESSAGES: &[u8] = include_bytes!("../../../tests/fixtures/dbus/messages.bin");
const DUMP: &[u8] = include_bytes!("../../../tests/fixtures/evo_check/dump.golden");
const ALPS_CONFIG: &[u8] = include_bytes!("../../../tests/fixtures/evo_check/alps-config.golden");
const USAGE_GOLDEN: &str = include_str!("../../../tests/fixtures/evo_check/usage.golden");
const OWNER: &str = include_str!("../../../tests/fixtures/evo_check/alps/owner.txt");
const REQUESTS: &[u8] = include_bytes!("../../../tests/fixtures/evo_check/alps/requests.bin");
const SIGNALS: &[u8] = include_bytes!("../../../tests/fixtures/evo_check/alps/signals.bin");
const MONITOR: &str = include_str!("../../../tests/fixtures/evo_check/alps/monitor.golden");

fn split(wire: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let mut at = 0;
    while at < wire.len() {
        let n = Message::wire_length(&wire[at..]).expect("valid").expect("whole");
        out.push(&wire[at..at + n]);
        at += n;
    }
    out
}

fn same(name: &str, got: &[u8], expected: &[u8]) {
    if got == expected {
        return;
    }
    let at = got.iter().zip(expected).take_while(|(a, b)| a == b).count();
    let show = |s: &[u8]| String::from_utf8_lossy(&s[at.saturating_sub(60)..(at + 60).min(s.len())]).into_owned();
    panic!(
        "{name} differs from the C++ at byte {at}:\n  C++:  {:?}\n  Rust: {:?}",
        show(expected),
        show(got)
    );
}

#[test]
fn the_dump_of_every_message_is_the_cpp_s() {
    let mut out = Vec::new();
    for (i, bytes) in split(MESSAGES).into_iter().enumerate() {
        let (m, _) = Message::parse(bytes).expect("valid").expect("whole");
        out.extend_from_slice(format!("# message {i}\n").as_bytes());
        dump(&mut out, &m.body, 0);
    }
    same("dump.golden", &out, DUMP);
}

#[test]
fn the_alps_configuration_summary_of_every_case_is_the_cpp_s() {
    let mut out = Vec::new();
    for (i, text) in ALPS_CASES.iter().enumerate() {
        out.extend_from_slice(format!("# alps-config-{}.xml\n", i + 1).as_bytes());
        out.extend_from_slice(&alps_config_summary(Some(text)));
    }
    // Past the size limit, a folder and a missing file: all three unreadable to readText.
    for case in ["262145 bytes", "a folder", "missing"] {
        out.extend_from_slice(format!("# {case}\n").as_bytes());
        out.extend_from_slice(&alps_config_summary(None));
    }
    same("alps-config.golden", &out, ALPS_CONFIG);
}

#[test]
fn the_usage_is_the_cpp_s() {
    assert_eq!(USAGE, USAGE_GOLDEN);
}

#[test]
fn the_alps_requests_are_libdbus_s_bytes() {
    let owner = OWNER.trim_end();
    let recorded = split(REQUESTS);
    let built = [
        introspect_request(owner, PATH),
        read_configuration_request(owner, 0, DATA_INTERFACE),
        read_configuration_request(owner, 1, DATA_INTERFACE),
    ];
    assert_eq!(recorded.len(), built.len());
    for (i, (bytes, m)) in recorded.iter().zip(&built).enumerate() {
        same(&format!("request {i}"), &m.marshal(), bytes);
    }
}

#[test]
fn the_alps_link_logs_the_signals_as_the_cpp_does() {
    let mut link = AlpsMonitor::new(OWNER.trim_end(), 0);
    let mut log = String::new();
    for bytes in split(SIGNALS) {
        let (m, _) = Message::parse(bytes).expect("valid").expect("whole");
        for line in link.on_message(&m) {
            log += &format!("[SPP] {line}\n");
        }
    }
    same("monitor.golden", log.as_bytes(), MONITOR.as_bytes());
    assert!(!link.active, "the service giving up its name stops the link");
}

#[test]
fn a_configuration_read_takes_only_its_own_answer() {
    let mut link = AlpsMonitor::new(OWNER.trim_end(), 0);
    link.waiting = Some((5, DATA_INTERFACE));
    for bytes in split(SIGNALS) {
        let (m, _) = Message::parse(bytes).expect("valid").expect("whole");
        link.on_message(&m);
    }
    // The script's unasked answer for instance 5 carries one byte, 0x01.
    assert_eq!(link.waiting, None);
    assert_eq!((link.status, link.data.as_slice()), (0, &[1u8][..]));
}
