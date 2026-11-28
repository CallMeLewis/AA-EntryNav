// Copyright (c) 2026 CallMeLewis
// evo-ric against what the C++ tools/evo_ric.cpp recorded (tests/test_ric_golden.cpp): the line for each message of
// tests/fixtures/dbus/messages.bin, the watch's output for all of them with and without a size limit, the usage
// text and the self test's output.

use aa_dbus::Message;
use evo_ric::{describe, Watch, USAGE};

const MESSAGES: &[u8] = include_bytes!("../../../tests/fixtures/dbus/messages.bin");
const DESCRIBE: &[u8] = include_bytes!("../../../tests/fixtures/evo_ric/describe.golden");
const WATCH: &[u8] = include_bytes!("../../../tests/fixtures/evo_ric/watch.golden");
const WATCH_SMALL: &[u8] = include_bytes!("../../../tests/fixtures/evo_ric/watch-small.golden");
const USAGE_GOLDEN: &str = include_str!("../../../tests/fixtures/evo_ric/usage.golden");
const SELFTEST: &str = include_str!("../../../tests/fixtures/evo_ric/selftest.golden");

fn messages() -> Vec<Message> {
    let mut out = Vec::new();
    let mut at = 0;
    while at < MESSAGES.len() {
        let (m, used) = Message::parse(&MESSAGES[at..])
            .unwrap_or_else(|e| panic!("message {} at byte {at}: {e}", out.len()))
            .unwrap_or_else(|| panic!("message {} at byte {at} is cut short", out.len()));
        out.push(m);
        at += used;
    }
    out
}

/// Where two outputs first differ, as a readable failure.
fn same(name: &str, got: &[u8], expected: &[u8]) {
    if got == expected {
        return;
    }
    let at = got.iter().zip(expected).take_while(|(a, b)| a == b).count();
    let line = expected[..at].iter().filter(|&&b| b == b'\n').count() + 1;
    let show = |s: &[u8]| String::from_utf8_lossy(&s[at.saturating_sub(80)..(at + 80).min(s.len())]).into_owned();
    panic!(
        "{name} differs from the C++ at byte {at} (line {line}), {} bytes against {}:\n  C++:  {}\n  Rust: {}",
        expected.len(),
        got.len(),
        show(expected),
        show(got)
    );
}

#[test]
fn every_message_is_described_as_the_cpp_does() {
    let mut out = Vec::new();
    for (i, m) in messages().iter().enumerate() {
        out.extend_from_slice(&describe(m, i as u64 * 37));
        out.push(b'\n');
    }
    same("describe.golden", &out, DESCRIBE);
}

fn watch_all(max_bytes: usize) -> Vec<u8> {
    let mut w = Watch::new(Vec::new(), max_bytes);
    for (i, m) in messages().iter().enumerate() {
        w.on_message(m, i as u64 * 37);
    }
    let mut out = w.file;
    out.extend_from_slice(
        format!(
            "# counts: {} calls, {} answers, {} signals, {} bytes, {} dropped, full {}\n# answers matched from:",
            w.calls, w.answers, w.signals, w.bytes, w.dropped, w.full as u8
        )
        .as_bytes(),
    );
    for name in &w.answering {
        out.extend_from_slice(format!(" {name}").as_bytes());
    }
    out.push(b'\n');
    out
}

#[test]
fn the_watch_writes_what_the_cpp_writes() {
    same("watch.golden", &watch_all(8 << 20), WATCH);
}

#[test]
fn the_watch_stops_at_its_size_limit_as_the_cpp_does() {
    same("watch-small.golden", &watch_all(3000), WATCH_SMALL);
}

#[test]
fn a_callee_by_unique_name_gets_two_answer_rules_once() {
    let mut w = Watch::new(Vec::new(), 1 << 20);
    for m in messages() {
        w.on_message(&m, 0);
    }
    // 16 callees past the three services, two rules each, never twice.
    assert_eq!(w.rules.len(), 32);
    assert_eq!(w.rules[0], "eavesdrop=true,type='method_return',sender=':1.50'");
    assert_eq!(w.rules[1], "eavesdrop=true,type='error',sender=':1.50'");
}

#[test]
fn usage_and_self_test_as_the_cpp_prints_them() {
    assert_eq!(USAGE, USAGE_GOLDEN);
    assert_eq!(SELFTEST.lines().last(), Some(evo_ric::SELFTEST_PASS));
}
