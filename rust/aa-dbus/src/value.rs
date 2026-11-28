// Copyright (c) 2026 CallMeLewis
//! D-Bus values and their wire format (the D-Bus specification's "Marshaling"), in both byte orders.

/// One D-Bus value. An array keeps its element signature, so that an empty one keeps its type.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Byte(u8),
    Boolean(bool),
    Int16(i16),
    Uint16(u16),
    Int32(i32),
    Uint32(u32),
    Int64(i64),
    Uint64(u64),
    Double(f64),
    String(String),
    ObjectPath(String),
    Signature(String),
    /// An index into the message's file descriptors. Nothing here passes descriptors.
    UnixFd(u32),
    Array(String, Vec<Value>),
    Struct(Vec<Value>),
    DictEntry(Box<Value>, Box<Value>),
    Variant(Box<Value>),
}

impl Value {
    /// The value's type code: its signature's first character.
    pub fn code(&self) -> u8 {
        match self {
            Value::Byte(_) => b'y',
            Value::Boolean(_) => b'b',
            Value::Int16(_) => b'n',
            Value::Uint16(_) => b'q',
            Value::Int32(_) => b'i',
            Value::Uint32(_) => b'u',
            Value::Int64(_) => b'x',
            Value::Uint64(_) => b't',
            Value::Double(_) => b'd',
            Value::String(_) => b's',
            Value::ObjectPath(_) => b'o',
            Value::Signature(_) => b'g',
            Value::UnixFd(_) => b'h',
            Value::Array(..) => b'a',
            Value::Struct(_) => b'(',
            Value::DictEntry(..) => b'{',
            Value::Variant(_) => b'v',
        }
    }

    /// The value's signature: one complete type.
    pub fn signature(&self) -> String {
        match self {
            Value::Array(element, _) => format!("a{element}"),
            Value::Struct(fields) => format!("({})", fields.iter().map(Value::signature).collect::<String>()),
            Value::DictEntry(key, value) => format!("{{{}{}}}", key.signature(), value.signature()),
            other => (other.code() as char).to_string(),
        }
    }

    /// A byte array.
    pub fn bytes(data: &[u8]) -> Value {
        Value::Array("y".into(), data.iter().map(|&b| Value::Byte(b)).collect())
    }

    /// The bytes of a byte array, or None for any other value.
    pub fn as_bytes(&self) -> Option<Vec<u8>> {
        match self {
            Value::Array(element, items) if element == "y" => Some(
                items
                    .iter()
                    .map(|v| match v {
                        Value::Byte(b) => *b,
                        _ => 0,
                    })
                    .collect(),
            ),
            _ => None,
        }
    }

    /// The text of a string, object path or signature.
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::String(s) | Value::ObjectPath(s) | Value::Signature(s) => Some(s),
            _ => None,
        }
    }
}

/// The alignment of a type code on the wire.
pub(crate) fn alignment(code: u8) -> usize {
    match code {
        b'n' | b'q' => 2,
        b'b' | b'i' | b'u' | b's' | b'o' | b'a' | b'h' => 4,
        b'x' | b't' | b'd' | b'(' | b'{' => 8,
        _ => 1,
    }
}

/// The end of the complete type that starts at `sig[start]`, or None if it is not one.
fn complete_type_end(sig: &[u8], start: usize, depth: usize) -> Option<usize> {
    if depth > 64 {
        return None;
    }
    match *sig.get(start)? {
        b'y' | b'b' | b'n' | b'q' | b'i' | b'u' | b'x' | b't' | b'd' | b's' | b'o' | b'g' | b'h' | b'v' => {
            Some(start + 1)
        }
        b'a' => complete_type_end(sig, start + 1, depth + 1),
        b'(' => {
            let mut at = start + 1;
            if sig.get(at) == Some(&b')') {
                return None;
            }
            while sig.get(at) != Some(&b')') {
                at = complete_type_end(sig, at, depth + 1)?;
            }
            Some(at + 1)
        }
        b'{' => {
            // A dict entry: a basic key and one complete value, only as an array's element.
            if start == 0 || sig[start - 1] != b'a' {
                return None;
            }
            let key = *sig.get(start + 1)?;
            if !b"ybnqiuxtdsogh".contains(&key) {
                return None;
            }
            let end = complete_type_end(sig, start + 2, depth + 1)?;
            (sig.get(end) == Some(&b'}')).then_some(end + 1)
        }
        _ => None,
    }
}

/// The complete types of a signature, in order, or None if it is not a valid signature.
pub fn single_types(sig: &str) -> Option<Vec<&str>> {
    let bytes = sig.as_bytes();
    let mut out = Vec::new();
    let mut at = 0;
    while at < bytes.len() {
        let end = complete_type_end(bytes, at, 0)?;
        out.push(&sig[at..end]);
        at = end;
    }
    Some(out)
}

/// Reads values from a message's bytes. Offsets count from the message's first byte, which is 8-aligned.
pub(crate) struct Reader<'a> {
    pub buf: &'a [u8],
    pub at: usize,
    pub big: bool,
}

impl<'a> Reader<'a> {
    fn align(&mut self, to: usize) -> Result<(), String> {
        let next = self.at.div_ceil(to) * to;
        if next > self.buf.len() {
            return Err("cut short".into());
        }
        if self.buf[self.at..next].iter().any(|&b| b != 0) {
            return Err("padding that is not zero".into());
        }
        self.at = next;
        Ok(())
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], String> {
        let end = self
            .at
            .checked_add(n)
            .filter(|&e| e <= self.buf.len())
            .ok_or("cut short")?;
        let out = &self.buf[self.at..end];
        self.at = end;
        Ok(out)
    }

    fn fixed<const N: usize>(&mut self) -> Result<[u8; N], String> {
        self.align(N)?;
        let mut out: [u8; N] = self.take(N)?.try_into().expect("N bytes");
        if !self.big {
            out.reverse();
        }
        Ok(out)
    }

    pub fn u8(&mut self) -> Result<u8, String> {
        Ok(self.take(1)?[0])
    }

    pub fn u32(&mut self) -> Result<u32, String> {
        Ok(u32::from_be_bytes(self.fixed::<4>()?))
    }

    fn text(&mut self, length: usize) -> Result<String, String> {
        let bytes = self.take(length)?;
        if self.take(1)? != [0] {
            return Err("a string without its NUL".into());
        }
        if bytes.contains(&0) {
            return Err("a NUL inside a string".into());
        }
        String::from_utf8(bytes.to_vec()).map_err(|_| "a string that is not UTF-8".into())
    }

    fn signature_text(&mut self) -> Result<String, String> {
        let length = self.u8()? as usize;
        let text = self.text(length)?;
        single_types(&text).ok_or("an invalid signature")?;
        Ok(text)
    }

    /// One value of the complete type `sig`.
    pub fn value(&mut self, sig: &str, depth: usize) -> Result<Value, String> {
        if depth > 64 {
            return Err("nested too deeply".into());
        }
        let code = sig.as_bytes()[0];
        Ok(match code {
            b'y' => Value::Byte(self.u8()?),
            b'b' => match self.u32()? {
                0 => Value::Boolean(false),
                1 => Value::Boolean(true),
                other => return Err(format!("boolean {other}")),
            },
            b'n' => Value::Int16(i16::from_be_bytes(self.fixed::<2>()?)),
            b'q' => Value::Uint16(u16::from_be_bytes(self.fixed::<2>()?)),
            b'i' => Value::Int32(i32::from_be_bytes(self.fixed::<4>()?)),
            b'u' => Value::Uint32(self.u32()?),
            b'h' => Value::UnixFd(self.u32()?),
            b'x' => Value::Int64(i64::from_be_bytes(self.fixed::<8>()?)),
            b't' => Value::Uint64(u64::from_be_bytes(self.fixed::<8>()?)),
            b'd' => Value::Double(f64::from_be_bytes(self.fixed::<8>()?)),
            b's' => {
                let length = self.u32()? as usize;
                Value::String(self.text(length)?)
            }
            b'o' => {
                let length = self.u32()? as usize;
                let path = self.text(length)?;
                if !path.starts_with('/') {
                    return Err("an object path without its /".into());
                }
                Value::ObjectPath(path)
            }
            b'g' => Value::Signature(self.signature_text()?),
            b'v' => {
                let inner = self.signature_text()?;
                if single_types(&inner).map(|t| t.len()) != Some(1) {
                    return Err("a variant of other than one type".into());
                }
                Value::Variant(Box::new(self.value(&inner, depth + 1)?))
            }
            b'a' => {
                let length = self.u32()? as usize;
                if length > 64 << 20 {
                    return Err("an array over 64 MiB".into());
                }
                let element = &sig[1..];
                self.align(alignment(element.as_bytes()[0]))?;
                let end = self
                    .at
                    .checked_add(length)
                    .filter(|&e| e <= self.buf.len())
                    .ok_or("cut short")?;
                let mut items = Vec::new();
                while self.at < end {
                    items.push(self.value(element, depth + 1)?);
                }
                if self.at != end {
                    return Err("an array's last element past its length".into());
                }
                Value::Array(element.into(), items)
            }
            b'(' => {
                self.align(8)?;
                let inner = single_types(&sig[1..sig.len() - 1]).ok_or("an invalid struct")?;
                let mut fields = Vec::with_capacity(inner.len());
                for field in inner {
                    fields.push(self.value(field, depth + 1)?);
                }
                Value::Struct(fields)
            }
            b'{' => {
                self.align(8)?;
                let inner = single_types(&sig[1..sig.len() - 1]).ok_or("an invalid dict entry")?;
                let key = self.value(inner[0], depth + 1)?;
                let value = self.value(inner[1], depth + 1)?;
                Value::DictEntry(Box::new(key), Box::new(value))
            }
            other => return Err(format!("type code {:?}", other as char)),
        })
    }
}

/// Writes values in little-endian order, as libdbus does on ARM and x86. Offsets count from an 8-aligned start.
pub(crate) struct Writer {
    pub out: Vec<u8>,
}

impl Writer {
    pub fn align(&mut self, to: usize) {
        while !self.out.len().is_multiple_of(to) {
            self.out.push(0);
        }
    }

    pub fn u32(&mut self, v: u32) {
        self.align(4);
        self.out.extend_from_slice(&v.to_le_bytes());
    }

    fn text(&mut self, s: &str) {
        self.u32(s.len() as u32);
        self.out.extend_from_slice(s.as_bytes());
        self.out.push(0);
    }

    pub fn signature(&mut self, s: &str) {
        self.out.push(s.len() as u8);
        self.out.extend_from_slice(s.as_bytes());
        self.out.push(0);
    }

    pub fn value(&mut self, v: &Value) {
        match v {
            Value::Byte(b) => self.out.push(*b),
            Value::Boolean(b) => self.u32(*b as u32),
            Value::Int16(n) => {
                self.align(2);
                self.out.extend_from_slice(&n.to_le_bytes());
            }
            Value::Uint16(n) => {
                self.align(2);
                self.out.extend_from_slice(&n.to_le_bytes());
            }
            Value::Int32(n) => {
                self.align(4);
                self.out.extend_from_slice(&n.to_le_bytes());
            }
            Value::Uint32(n) | Value::UnixFd(n) => self.u32(*n),
            Value::Int64(n) => {
                self.align(8);
                self.out.extend_from_slice(&n.to_le_bytes());
            }
            Value::Uint64(n) => {
                self.align(8);
                self.out.extend_from_slice(&n.to_le_bytes());
            }
            Value::Double(d) => {
                self.align(8);
                self.out.extend_from_slice(&d.to_le_bytes());
            }
            Value::String(s) | Value::ObjectPath(s) => self.text(s),
            Value::Signature(s) => self.signature(s),
            Value::Variant(inner) => {
                self.signature(&inner.signature());
                self.value(inner);
            }
            Value::Array(element, items) => {
                self.u32(0);
                let length_at = self.out.len() - 4;
                self.align(alignment(element.as_bytes()[0]));
                let start = self.out.len();
                for item in items {
                    self.value(item);
                }
                let length = (self.out.len() - start) as u32;
                self.out[length_at..length_at + 4].copy_from_slice(&length.to_le_bytes());
            }
            Value::Struct(fields) => {
                self.align(8);
                for field in fields {
                    self.value(field);
                }
            }
            Value::DictEntry(key, value) => {
                self.align(8);
                self.value(key);
                self.value(value);
            }
        }
    }
}
