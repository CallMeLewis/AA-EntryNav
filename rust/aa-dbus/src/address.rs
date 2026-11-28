// Copyright (c) 2026 CallMeLewis
//! Bus addresses (`unix:path=...`, `unix:abstract=...`, several separated by `;`), as libdbus reads them.

/// A Unix socket to connect to: a file, or a name in Linux's abstract namespace.
#[derive(Clone, Debug, PartialEq)]
pub enum Address {
    Path(String),
    Abstract(String),
}

/// The Unix sockets an address names, in order. Other transports are left out; libdbus would try them, and the
/// tools only ever meet Unix sockets. Err holds libdbus's text when nothing usable is left.
pub fn parse_address(text: &str) -> Result<Vec<Address>, String> {
    let mut out = Vec::new();
    let mut unknown = false;
    for entry in text.split(';').filter(|e| !e.is_empty()) {
        let Some((method, rest)) = entry.split_once(':') else {
            return Err(format!("Address does not contain a colon: {entry}"));
        };
        if method != "unix" {
            unknown = true;
            continue;
        }
        let mut path = None;
        let mut name = None;
        for pair in rest.split(',').filter(|p| !p.is_empty()) {
            let Some((key, value)) = pair.split_once('=') else {
                return Err(format!("Address does not contain an equals sign: {pair}"));
            };
            let value = unescape(value).ok_or_else(|| format!("Invalid escape in address value: {value}"))?;
            match key {
                "path" => path = Some(value),
                "abstract" => name = Some(value),
                _ => {}
            }
        }
        match (path, name) {
            (Some(p), None) => out.push(Address::Path(p)),
            (None, Some(n)) => out.push(Address::Abstract(n)),
            _ => return Err("Must specify either path or abstract for unix: address".into()),
        }
    }
    if out.is_empty() {
        return Err(if unknown {
            "Unknown address type (examples of valid types are \"tcp\" and on UNIX \"unix\")".into()
        } else {
            "Empty address".into()
        });
    }
    Ok(out)
}

/// An address value with its %XX escapes undone.
fn unescape(value: &str) -> Option<String> {
    let bytes = value.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut at = 0;
    while at < bytes.len() {
        if bytes[at] == b'%' {
            let hex = std::str::from_utf8(bytes.get(at + 1..at + 3)?).ok()?;
            out.push(u8::from_str_radix(hex, 16).ok()?);
            at += 3;
        } else {
            out.push(bytes[at]);
            at += 1;
        }
    }
    String::from_utf8(out).ok()
}
