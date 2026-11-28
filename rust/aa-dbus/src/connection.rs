// Copyright (c) 2026 CallMeLewis
//! A blocking connection to a bus over its Unix socket: EXTERNAL authentication, Hello, calls and their answers,
//! and the messages that come in between, kept in order for `receive`.

use std::collections::VecDeque;
use std::io::{ErrorKind, Read, Write};
use std::os::unix::net::UnixStream;
use std::time::{Duration, Instant};

use crate::address::{parse_address, Address};
use crate::message::{BusError, Message, MESSAGE_ERROR, METHOD_RETURN};
use crate::value::Value;
use crate::{BUS_INTERFACE, BUS_NAME, BUS_PATH};

/// libdbus 1.6's text for a call that got no answer in time.
const NO_REPLY: &str = "Did not receive a reply. Possible causes include: the remote application did not send a \
                        reply, the message bus security policy blocked the reply, the reply timeout expired, or the \
                        network connection was broken.";

/// What `receive` found.
#[derive(Debug)]
pub enum Received {
    Message(Message),
    /// Nothing came in the time given.
    Nothing,
    /// The bus closed the connection, or it failed.
    Closed,
}

pub struct Connection {
    stream: UnixStream,
    inbox: Vec<u8>,
    queue: VecDeque<Message>,
    serial: u32,
    closed: bool,
    unique: String,
}

fn connect(address: &Address) -> Result<UnixStream, BusError> {
    let (name, result) = match address {
        Address::Path(path) => (path, UnixStream::connect(path)),
        Address::Abstract(name) => {
            #[cfg(any(target_os = "linux", target_os = "android"))]
            {
                use std::os::linux::net::SocketAddrExt;
                let result = std::os::unix::net::SocketAddr::from_abstract_name(name.as_bytes())
                    .and_then(|a| UnixStream::connect_addr(&a));
                (name, result)
            }
            #[cfg(not(any(target_os = "linux", target_os = "android")))]
            (name, Err(std::io::Error::from(ErrorKind::Unsupported)))
        }
    };
    result.map_err(|e| {
        let errno = e.raw_os_error().unwrap_or(0);
        let error = match errno {
            2 => "org.freedesktop.DBus.Error.FileNotFound",
            13 => "org.freedesktop.DBus.Error.AccessDenied",
            _ => "org.freedesktop.DBus.Error.Failed",
        };
        BusError::new(
            error,
            format!("Failed to connect to socket {name}: {}", evo_text::strerror(errno)),
        )
    })
}

impl Connection {
    /// Connects to the first socket of the address that answers and authenticates; no Hello yet (`register`).
    pub fn open(address: &str) -> Result<Connection, BusError> {
        let sockets = parse_address(address).map_err(|m| BusError::new("org.freedesktop.DBus.Error.BadAddress", m))?;
        let mut last = None;
        for socket in &sockets {
            match connect(socket) {
                Ok(stream) => {
                    let mut c = Connection {
                        stream,
                        inbox: Vec::new(),
                        queue: VecDeque::new(),
                        serial: 0,
                        closed: false,
                        unique: String::new(),
                    };
                    c.authenticate()?;
                    return Ok(c);
                }
                Err(e) => last = Some(e),
            }
        }
        Err(last.expect("parse_address gives at least one socket"))
    }

    /// Connects and says Hello, as dbus_connection_open_private and dbus_bus_register do.
    pub fn open_bus(address: &str) -> Result<Connection, BusError> {
        let mut c = Connection::open(address)?;
        c.register()?;
        Ok(c)
    }

    fn authenticate(&mut self) -> Result<(), BusError> {
        let failed = |why: &str| BusError::new("org.freedesktop.DBus.Error.AuthFailed", why);
        // SAFETY: getuid cannot fail.
        let uid = unsafe { libc::getuid() };
        let hex: String = uid.to_string().bytes().map(|b| format!("{b:02x}")).collect();
        self.stream
            .set_read_timeout(Some(Duration::from_secs(25)))
            .map_err(|e| failed(&e.to_string()))?;
        let hello = format!("\0AUTH EXTERNAL {hex}\r\n");
        self.stream
            .write_all(hello.as_bytes())
            .map_err(|e| failed(&e.to_string()))?;
        let mut line = Vec::new();
        let mut byte = [0u8; 1];
        while !line.ends_with(b"\r\n") {
            match self.stream.read(&mut byte) {
                Ok(1) => line.push(byte[0]),
                _ => return Err(failed("Did not receive a reply to authentication")),
            }
            if line.len() > 512 {
                return Err(failed("Authentication answer too long"));
            }
        }
        if !line.starts_with(b"OK ") {
            return Err(failed("Authentication refused"));
        }
        self.stream.write_all(b"BEGIN\r\n").map_err(|e| failed(&e.to_string()))
    }

    /// Hello: the connection's unique name.
    pub fn register(&mut self) -> Result<(), BusError> {
        let reply = self.call(bus_call("Hello"), 25_000)?;
        self.unique = reply.first_text().unwrap_or_default().to_string();
        Ok(())
    }

    /// The connection's unique name, from Hello.
    pub fn unique_name(&self) -> &str {
        &self.unique
    }

    /// Sends a message with the next serial, which it returns.
    pub fn send(&mut self, message: &mut Message) -> Result<u32, BusError> {
        self.serial = self.serial.wrapping_add(1).max(1);
        message.serial = self.serial;
        let bytes = message.marshal();
        self.stream.write_all(&bytes).map_err(|e| {
            self.closed = true;
            BusError::new("org.freedesktop.DBus.Error.Disconnected", e.to_string())
        })?;
        Ok(self.serial)
    }

    /// Sends a call and waits for its answer, keeping what else comes for `receive`. An error answer, or none in
    /// time, gives Err, as dbus_connection_send_with_reply_and_block does.
    pub fn call(&mut self, mut message: Message, timeout_ms: u64) -> Result<Message, BusError> {
        let serial = self.send(&mut message)?;
        self.wait_reply(serial, timeout_ms)
    }

    /// The answer to the call that had `serial`, the other messages kept in order.
    pub fn wait_reply(&mut self, serial: u32, timeout_ms: u64) -> Result<Message, BusError> {
        if let Some(at) = self.queue.iter().position(|m| is_reply(m, serial)) {
            return answer(self.queue.remove(at).expect("found"));
        }
        let until = Instant::now() + Duration::from_millis(timeout_ms);
        loop {
            let left = until.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err(BusError::new("org.freedesktop.DBus.Error.NoReply", NO_REPLY));
            }
            match self.read_one(left) {
                Ok(Some(m)) if is_reply(&m, serial) => return answer(m),
                Ok(Some(m)) => self.queue.push_back(m),
                Ok(None) => {}
                Err(()) => {
                    return Err(BusError::new(
                        "org.freedesktop.DBus.Error.Disconnected",
                        "Connection was disconnected before a reply was received",
                    ))
                }
            }
        }
    }

    /// The next message: one kept earlier, or one read within `timeout`.
    pub fn receive(&mut self, timeout: Duration) -> Received {
        if let Some(m) = self.queue.pop_front() {
            return Received::Message(m);
        }
        match self.read_one(timeout) {
            Ok(Some(m)) => Received::Message(m),
            Ok(None) => Received::Nothing,
            Err(()) => Received::Closed,
        }
    }

    /// Whether messages are kept, ready for `receive` without reading.
    pub fn has_queued(&self) -> bool {
        !self.queue.is_empty()
    }

    /// One message from the socket, waiting at most `timeout`. Err when the connection is gone.
    fn read_one(&mut self, timeout: Duration) -> Result<Option<Message>, ()> {
        let until = Instant::now() + timeout;
        loop {
            match Message::parse(&self.inbox) {
                Ok(Some((m, used))) => {
                    self.inbox.drain(..used);
                    return Ok(Some(m));
                }
                Ok(None) => {}
                Err(_) => {
                    // libdbus drops a connection that sends what it cannot read.
                    self.closed = true;
                }
            }
            if self.closed {
                return Err(());
            }
            let left = until.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Ok(None);
            }
            if self
                .stream
                .set_read_timeout(Some(left.max(Duration::from_millis(1))))
                .is_err()
            {
                self.closed = true;
                continue;
            }
            let mut chunk = [0u8; 16384];
            match self.stream.read(&mut chunk) {
                Ok(0) => self.closed = true,
                Ok(n) => self.inbox.extend_from_slice(&chunk[..n]),
                Err(e)
                    if matches!(
                        e.kind(),
                        ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted
                    ) =>
                {
                    if e.kind() != ErrorKind::Interrupted {
                        return Ok(None);
                    }
                }
                Err(_) => self.closed = true,
            }
        }
    }

    /// A call to the bus daemon itself, with a string argument or none.
    pub fn ask_bus(&mut self, member: &str, arg: Option<&str>, timeout_ms: u64) -> Result<Message, BusError> {
        let mut m = bus_call(member);
        if let Some(a) = arg {
            m.append(Value::String(a.into()));
        }
        self.call(m, timeout_ms)
    }

    /// AddMatch, waiting for the bus's answer (dbus_bus_add_match with an error to fill).
    pub fn add_match(&mut self, rule: &str) -> Result<(), BusError> {
        self.ask_bus("AddMatch", Some(rule), 25_000).map(|_| ())
    }

    /// AddMatch without waiting (dbus_bus_add_match with no error): the bus's answer comes as a message.
    pub fn add_match_no_wait(&mut self, rule: &str) -> Result<(), BusError> {
        let mut m = bus_call("AddMatch").with([Value::String(rule.into())]);
        self.send(&mut m).map(|_| ())
    }

    /// RequestName: the bus's answer (1 when this connection now owns the name).
    pub fn request_name(&mut self, name: &str, flags: u32) -> Result<u32, BusError> {
        let m = bus_call("RequestName").with([Value::String(name.into()), Value::Uint32(flags)]);
        let reply = self.call(m, 25_000)?;
        Ok(match reply.body.first() {
            Some(Value::Uint32(n)) => *n,
            _ => 0,
        })
    }
}

fn bus_call(member: &str) -> Message {
    Message::method_call(Some(BUS_NAME), BUS_PATH, Some(BUS_INTERFACE), member)
}

fn is_reply(m: &Message, serial: u32) -> bool {
    (m.kind == METHOD_RETURN || m.kind == MESSAGE_ERROR) && m.reply_serial() == serial
}

fn answer(m: Message) -> Result<Message, BusError> {
    if m.kind == MESSAGE_ERROR {
        let name = m
            .error_name()
            .unwrap_or("org.freedesktop.DBus.Error.Failed")
            .to_string();
        let text = m.first_text().map(str::to_owned).unwrap_or_else(|| name.clone());
        return Err(BusError { name, message: text });
    }
    Ok(m)
}
