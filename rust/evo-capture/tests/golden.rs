// Copyright (c) 2026 CallMeLewis
//! The Rust evo-capture against what tests/test_capture_golden.cpp recorded of the C++ one, in
//! tests/fixtures/evo_capture: the same summary for the same packets, the same pcap bytes, the same self test output.
//! The fixtures are compiled in, so the test binary runs the same on the PC and on ARM under QEMU.

use evo_capture::{read_pcap, selftest, PcapWriter, Summary};

const CORPUS: &[u8] = include_bytes!("../../../tests/fixtures/evo_capture/corpus.pcap");
const SUMMARY: &str = include_str!("../../../tests/fixtures/evo_capture/summary.golden");
const SELFTEST: &str = include_str!("../../../tests/fixtures/evo_capture/selftest.golden");
/// A record's microseconds field names the interface it came in on, as tests/test_capture_golden.cpp writes it.
const INTERFACES: [&str; 3] = ["vbr0.73", "eth0", "wlan0"];

/// The first line where two texts differ, for a readable failure.
fn first_difference(want: &str, got: &str) -> String {
    for (i, (w, g)) in want.lines().zip(got.lines()).enumerate() {
        if w != g {
            return format!("line {}:\n  C++:  {w}\n  Rust: {g}", i + 1);
        }
    }
    format!(
        "lengths differ: C++ {} lines, Rust {}",
        want.lines().count(),
        got.lines().count()
    )
}

#[test]
fn the_corpus_summary_matches_the_cpp() {
    let records = read_pcap(CORPUS).expect("tests/fixtures/evo_capture/corpus.pcap is a pcap of raw IPv4");
    assert!(records.len() > 300, "the corpus lost its packets: {}", records.len());
    let mut summary = Summary::default();
    for record in &records {
        summary.add_ipv4(INTERFACES[record.usec as usize], &record.data);
    }
    let mut text = Vec::new();
    summary.print(&mut text).unwrap();
    let text = String::from_utf8(text).unwrap();
    assert!(
        text == SUMMARY,
        "the summary differs from the C++'s, {}",
        first_difference(SUMMARY, &text)
    );
}

#[test]
fn the_pcap_bytes_match_the_cpp() {
    let records = read_pcap(CORPUS).unwrap();
    let mut pcap = PcapWriter::new(Vec::new()).unwrap();
    for record in &records {
        pcap.add(record.sec, record.usec, &record.data, record.len as usize)
            .unwrap();
    }
    assert_eq!(pcap.bytes as usize, CORPUS.len());
    assert!(
        pcap.finish().unwrap() == CORPUS,
        "the pcap writer's bytes differ from the C++'s"
    );
}

#[test]
fn the_selftest_output_matches_the_cpp() {
    let mut out = Vec::new();
    assert!(selftest(&mut out).unwrap(), "the self test failed");
    let text = String::from_utf8(out).unwrap();
    assert!(
        text == SELFTEST,
        "the self test prints differently, {}",
        first_difference(SELFTEST, &text)
    );
}
