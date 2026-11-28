// Copyright (c) 2026 CallMeLewis
//! aa-dbus: the small blocking D-Bus client of AA-EntryNav's Rust programs (docs/rust-port.md, step 3).
//!
//! It speaks the bus's wire protocol itself over its Unix socket, so it needs no libdbus and works in a static musl
//! program. It covers what the tools and the service use of D-Bus, and no more: EXTERNAL authentication, `Hello`,
//! method calls and their answers, signals, match rules, and every type of the wire format in both byte orders.
//!
//! Messages it builds are laid out as libdbus 1.6 lays them out (the order of the header fields, the flags of an
//! answer), so a BMW program receives the same bytes from a Rust port as from its C++ original. The fixtures in
//! tests/fixtures/dbus check this against libdbus itself.

mod address;
#[cfg(unix)]
mod connection;
mod message;
mod value;

pub use address::{parse_address, Address};
#[cfg(unix)]
pub use connection::{Connection, Received};
pub use message::{
    BusError, Message, FIELD_DESTINATION, FIELD_ERROR_NAME, FIELD_INTERFACE, FIELD_MEMBER, FIELD_PATH,
    FIELD_REPLY_SERIAL, FIELD_SENDER, FIELD_SIGNATURE, FIELD_UNIX_FDS, FLAG_NO_REPLY_EXPECTED, MESSAGE_ERROR,
    METHOD_CALL, METHOD_RETURN, SIGNAL,
};
pub use value::{single_types, Value};

/// The bus daemon's own name, path and interface.
pub const BUS_NAME: &str = "org.freedesktop.DBus";
pub const BUS_PATH: &str = "/org/freedesktop/DBus";
pub const BUS_INTERFACE: &str = "org.freedesktop.DBus";
/// The interface of the messages libdbus makes up itself (Disconnected); never on the wire.
pub const LOCAL_INTERFACE: &str = "org.freedesktop.DBus.Local";
/// RequestName's flag that refuses to queue for a name another connection owns.
pub const NAME_FLAG_DO_NOT_QUEUE: u32 = 4;
