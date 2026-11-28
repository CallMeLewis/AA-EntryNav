// Copyright (c) 2026 CallMeLewis
//! D-Bus messages: the fixed header, the header fields and the body, read in either byte order and written as
//! libdbus 1.6 writes them.

use crate::value::{single_types, Reader, Value, Writer};

pub const METHOD_CALL: u8 = 1;
pub const METHOD_RETURN: u8 = 2;
pub const MESSAGE_ERROR: u8 = 3;
pub const SIGNAL: u8 = 4;

pub const FLAG_NO_REPLY_EXPECTED: u8 = 1;

pub const FIELD_PATH: u8 = 1;
pub const FIELD_INTERFACE: u8 = 2;
pub const FIELD_MEMBER: u8 = 3;
pub const FIELD_ERROR_NAME: u8 = 4;
pub const FIELD_REPLY_SERIAL: u8 = 5;
pub const FIELD_DESTINATION: u8 = 6;
pub const FIELD_SENDER: u8 = 7;
pub const FIELD_SIGNATURE: u8 = 8;
pub const FIELD_UNIX_FDS: u8 = 9;

/// The largest message the bus passes (the specification's limit).
const MAX_MESSAGE: usize = 128 << 20;

/// An error, as libdbus's DBusError holds it: a name and a text.
#[derive(Clone, Debug, PartialEq)]
pub struct BusError {
    pub name: String,
    pub message: String,
}

impl BusError {
    pub fn new(name: &str, message: impl Into<String>) -> BusError {
        BusError {
            name: name.into(),
            message: message.into(),
        }
    }
}

impl std::fmt::Display for BusError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}

/// One message. The header fields keep their order, as libdbus writes and keeps them.
#[derive(Clone, Debug, PartialEq)]
pub struct Message {
    pub big_endian: bool,
    pub kind: u8,
    pub flags: u8,
    pub serial: u32,
    pub fields: Vec<(u8, Value)>,
    pub body: Vec<Value>,
}

impl Message {
    fn empty(kind: u8) -> Message {
        Message {
            big_endian: false,
            kind,
            flags: 0,
            serial: 0,
            fields: Vec::new(),
            body: Vec::new(),
        }
    }

    /// A method call, its fields in libdbus's order: path, destination, interface, member.
    pub fn method_call(destination: Option<&str>, path: &str, interface: Option<&str>, member: &str) -> Message {
        let mut m = Message::empty(METHOD_CALL);
        m.fields.push((FIELD_PATH, Value::ObjectPath(path.into())));
        if let Some(d) = destination {
            m.fields.push((FIELD_DESTINATION, Value::String(d.into())));
        }
        if let Some(i) = interface {
            m.fields.push((FIELD_INTERFACE, Value::String(i.into())));
        }
        m.fields.push((FIELD_MEMBER, Value::String(member.into())));
        m
    }

    /// A signal: path, interface, member.
    pub fn signal(path: &str, interface: &str, member: &str) -> Message {
        let mut m = Message::empty(SIGNAL);
        m.fields.push((FIELD_PATH, Value::ObjectPath(path.into())));
        m.fields.push((FIELD_INTERFACE, Value::String(interface.into())));
        m.fields.push((FIELD_MEMBER, Value::String(member.into())));
        m
    }

    /// The answer to a call, as dbus_message_new_method_return makes it: to the caller, no reply expected.
    pub fn method_return(call: &Message) -> Message {
        let mut m = Message::empty(METHOD_RETURN);
        if let Some(sender) = call.sender() {
            m.fields.push((FIELD_DESTINATION, Value::String(sender.into())));
        }
        m.flags |= FLAG_NO_REPLY_EXPECTED;
        m.fields.push((FIELD_REPLY_SERIAL, Value::Uint32(call.serial)));
        m
    }

    /// An error answer, as dbus_message_new_error makes it, with its text as its one argument.
    pub fn error(call: &Message, name: &str, text: Option<&str>) -> Message {
        let mut m = Message::empty(MESSAGE_ERROR);
        if let Some(sender) = call.sender() {
            m.fields.push((FIELD_DESTINATION, Value::String(sender.into())));
        }
        m.fields.push((FIELD_ERROR_NAME, Value::String(name.into())));
        m.flags |= FLAG_NO_REPLY_EXPECTED;
        m.fields.push((FIELD_REPLY_SERIAL, Value::Uint32(call.serial)));
        if let Some(t) = text {
            m.append(Value::String(t.into()));
        }
        m
    }

    /// Adds an argument; the signature field follows, added after the others the first time, as libdbus does.
    pub fn append(&mut self, value: Value) -> &mut Message {
        self.body.push(value);
        let signature: String = self.body.iter().map(Value::signature).collect();
        match self.fields.iter_mut().find(|(code, _)| *code == FIELD_SIGNATURE) {
            Some(field) => field.1 = Value::Signature(signature),
            None => self.fields.push((FIELD_SIGNATURE, Value::Signature(signature))),
        }
        self
    }

    pub fn with(mut self, values: impl IntoIterator<Item = Value>) -> Message {
        for v in values {
            self.append(v);
        }
        self
    }

    pub fn set_no_reply(&mut self, on: bool) {
        if on {
            self.flags |= FLAG_NO_REPLY_EXPECTED;
        } else {
            self.flags &= !FLAG_NO_REPLY_EXPECTED;
        }
    }

    /// Sets a string field: in place if there, else after the others. None removes it.
    pub fn set_field(&mut self, code: u8, value: Option<Value>) {
        let at = self.fields.iter().position(|(c, _)| *c == code);
        match (at, value) {
            (Some(i), Some(v)) => self.fields[i].1 = v,
            (None, Some(v)) => self.fields.push((code, v)),
            (Some(i), None) => {
                self.fields.remove(i);
            }
            (None, None) => {}
        }
    }

    pub fn field(&self, code: u8) -> Option<&Value> {
        self.fields.iter().find(|(c, _)| *c == code).map(|(_, v)| v)
    }

    fn text_field(&self, code: u8) -> Option<&str> {
        self.field(code).and_then(Value::as_str)
    }

    pub fn path(&self) -> Option<&str> {
        self.text_field(FIELD_PATH)
    }
    pub fn interface(&self) -> Option<&str> {
        self.text_field(FIELD_INTERFACE)
    }
    pub fn member(&self) -> Option<&str> {
        self.text_field(FIELD_MEMBER)
    }
    pub fn error_name(&self) -> Option<&str> {
        self.text_field(FIELD_ERROR_NAME)
    }
    pub fn destination(&self) -> Option<&str> {
        self.text_field(FIELD_DESTINATION)
    }
    pub fn sender(&self) -> Option<&str> {
        self.text_field(FIELD_SENDER)
    }
    /// The body's signature, "" when the message has none (dbus_message_get_signature).
    pub fn signature(&self) -> &str {
        self.text_field(FIELD_SIGNATURE).unwrap_or("")
    }
    /// 0 when absent, as dbus_message_get_reply_serial gives.
    pub fn reply_serial(&self) -> u32 {
        match self.field(FIELD_REPLY_SERIAL) {
            Some(Value::Uint32(n)) => *n,
            _ => 0,
        }
    }

    pub fn is_signal(&self, interface: &str, member: &str) -> bool {
        self.kind == SIGNAL && self.interface() == Some(interface) && self.member() == Some(member)
    }

    /// The first argument as text, as libdbus reads an error's message.
    pub fn first_text(&self) -> Option<&str> {
        match self.body.first() {
            Some(Value::String(s)) => Some(s),
            _ => None,
        }
    }

    /// The message's length once complete, from its first 16 bytes; None until they are there.
    pub fn wire_length(buf: &[u8]) -> Result<Option<usize>, String> {
        if buf.len() < 16 {
            return Ok(None);
        }
        let big = match buf[0] {
            b'l' => false,
            b'B' => true,
            other => return Err(format!("byte order {other:#04x}")),
        };
        let word = |at: usize| {
            let b: [u8; 4] = buf[at..at + 4].try_into().expect("4 bytes");
            if big {
                u32::from_be_bytes(b)
            } else {
                u32::from_le_bytes(b)
            }
        };
        let fields = word(12) as usize;
        let body = word(4) as usize;
        let total = (16 + fields).div_ceil(8) * 8 + body;
        if total > MAX_MESSAGE {
            return Err(format!("a message of {total} bytes"));
        }
        Ok(Some(total))
    }

    /// One whole message from the start of `buf`, and its length; None while it is incomplete.
    pub fn parse(buf: &[u8]) -> Result<Option<(Message, usize)>, String> {
        let total = match Message::wire_length(buf)? {
            Some(t) if t <= buf.len() => t,
            _ => return Ok(None),
        };
        let buf = &buf[..total];
        let mut r = Reader {
            buf,
            at: 0,
            big: buf[0] == b'B',
        };
        r.at = 1;
        let kind = r.u8()?;
        let flags = r.u8()?;
        if r.u8()? != 1 {
            return Err("protocol version other than 1".into());
        }
        let body_length = r.u32()? as usize;
        let serial = r.u32()?;
        if serial == 0 {
            return Err("serial 0".into());
        }
        let fields_value = r.value("a(yv)", 0)?;
        let mut fields = Vec::new();
        if let Value::Array(_, items) = fields_value {
            for item in items {
                if let Value::Struct(mut pair) = item {
                    let value = pair.pop();
                    let code = pair.pop();
                    if let (Some(Value::Byte(code)), Some(Value::Variant(inner))) = (code, value) {
                        fields.push((code, *inner));
                    }
                }
            }
        }
        let header_end = r.at.div_ceil(8) * 8;
        if header_end + body_length != total {
            return Err("a body length unlike the message's".into());
        }
        r.at = header_end;
        let mut m = Message {
            big_endian: r.big,
            kind,
            flags,
            serial,
            fields,
            body: Vec::new(),
        };
        let signature = m.signature().to_string();
        let types = single_types(&signature).ok_or("an invalid body signature")?;
        for t in types {
            m.body.push(r.value(t, 0)?);
        }
        if r.at != total {
            return Err("bytes left after the body".into());
        }
        Ok(Some((m, total)))
    }

    /// The message's bytes, little-endian, as libdbus writes them.
    pub fn marshal(&self) -> Vec<u8> {
        let mut body = Writer { out: Vec::new() };
        for v in &self.body {
            body.value(v);
        }
        let mut w = Writer { out: Vec::new() };
        w.out.extend_from_slice(&[b'l', self.kind, self.flags, 1]);
        w.u32(body.out.len() as u32);
        w.u32(self.serial);
        w.u32(0);
        let start = w.out.len();
        for (code, value) in &self.fields {
            w.align(8);
            w.out.push(*code);
            w.signature(&value.signature());
            w.value(value);
        }
        let length = (w.out.len() - start) as u32;
        w.out[12..16].copy_from_slice(&length.to_le_bytes());
        w.align(8);
        w.out.extend_from_slice(&body.out);
        w.out
    }
}
