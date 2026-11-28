// Copyright (c) 2026 CallMeLewis
//! The Rust summary of a pcap evo-capture recorded in the car, every packet on interface "car", as
//! tests/test-capture-golden --summarize prints the C++ one. The two must be identical (docs/rust-port.md, step 1):
//!   cargo run --example summarize -- someip-YYYYmmdd-HHMMSS.pcap

use std::io::Write;
use std::process::ExitCode;

use evo_capture::{read_pcap, Summary};

fn main() -> ExitCode {
    let Some(path) = std::env::args_os().nth(1) else {
        eprintln!("usage: summarize FILE.pcap");
        return ExitCode::from(2);
    };
    let Some(records) = std::fs::read(&path).ok().as_deref().and_then(read_pcap) else {
        eprintln!(
            "{}: not a whole pcap of raw IPv4 from evo-capture",
            path.to_string_lossy()
        );
        return ExitCode::from(2);
    };
    let mut summary = Summary::default();
    for record in &records {
        summary.add_ipv4("car", &record.data);
    }
    let mut out = std::io::stdout().lock();
    if summary.print(&mut out).and_then(|()| out.flush()).is_err() {
        return ExitCode::from(1);
    }
    ExitCode::SUCCESS
}
