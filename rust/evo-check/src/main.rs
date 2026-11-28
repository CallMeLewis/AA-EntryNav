// Copyright (c) 2026 CallMeLewis
//! evo-check: read-only diagnostics on the car, the Rust port of tools/evo_check.cpp (docs/rust-port.md, step 4).
//! Same arguments, output and exit codes. No UUID registration, configuration write, video surface or TCP server:
//!   evo-check --scene | --context | --bluetooth | --monitor [seconds 1..60] | --help
//!   evo-check --dbus-names | --dbus-introspect <destination> <path> | --dbus-watch <path> [seconds 1..3600]
//! A developer tool, not part of a release (make dev-tools RUST_TOOLS=evo-check).

#[cfg(unix)]
mod link;
#[cfg(unix)]
mod process;
#[cfg(unix)]
mod program;

#[cfg(unix)]
fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(program::main() as u8)
}

#[cfg(not(unix))]
fn main() -> std::process::ExitCode {
    eprintln!("evo-check runs on Linux only");
    std::process::ExitCode::from(1)
}
