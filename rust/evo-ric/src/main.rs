// Copyright (c) 2026 CallMeLewis
//! evo-ric: passive recorder of the instrument cluster's guidance calls on the head unit's session bus, the Rust port
//! of tools/evo_ric.cpp (docs/rust-port.md, step 3). Same arguments, output, files and exit codes:
//!   evo-ric --names [--address A]                           well-known names on the bus, with their process
//!   evo-ric --watch [minutes] [--out DIR] [--max-mb N] [--address A]
//!   evo-ric --selftest                                      against the session bus of dbus-run-session (PC)
//!   evo-ric --help
//!
//! Nothing is sent to any BMW program: the only calls go to the bus itself (Hello, ListNames,
//! GetConnectionUnixProcessID and AddMatch), and an overheard call is never answered. Lowest CPU priority, one
//! thread, bounded memory, written where a restart doesn't reach, flushed and synced every 2 s, with a heartbeat
//! every 30 s.

#[cfg(unix)]
mod program;

#[cfg(unix)]
fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(program::main() as u8)
}

#[cfg(not(unix))]
fn main() -> std::process::ExitCode {
    eprintln!("evo-ric runs on Linux only");
    std::process::ExitCode::from(1)
}
