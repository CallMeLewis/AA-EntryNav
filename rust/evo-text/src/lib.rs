// Copyright (c) 2026 CallMeLewis
//! What the C++ tools get from the C library when they print or read text, so that their Rust ports give the same
//! bytes: printf's `%g`, strtol and strtoul as they parse a command line, and glibc's error texts.
//!
//! The head unit's C++ programs run on glibc 2.15. The Rust ports are static musl programs, and musl's texts and
//! number formatting are not always glibc's, so these never call the C library.

/// printf's `%.<precision>g` of `value`, as glibc prints it: `nan`, `-nan`, `inf`, `-inf` and `-0` included.
pub fn format_g(value: f64, precision: usize) -> String {
    let p = precision.max(1);
    if value.is_nan() {
        return if value.is_sign_negative() { "-nan" } else { "nan" }.into();
    }
    if value.is_infinite() {
        return if value < 0.0 { "-inf" } else { "inf" }.into();
    }
    // The exponent X that %e with precision P-1 would give, after its rounding.
    let scientific = format!("{:.*e}", p - 1, value);
    let (mantissa, exponent) = scientific.split_once('e').expect("Rust's {:e} has an exponent");
    let x: i32 = exponent.parse().expect("a decimal exponent");
    if (x as i64) < p as i64 && x >= -4 {
        let digits = (p as i64 - 1 - x as i64) as usize;
        strip_zeros(&format!("{value:.digits$}"))
    } else {
        let sign = if x < 0 { '-' } else { '+' };
        format!("{}e{sign}{:02}", strip_zeros(mantissa), x.unsigned_abs())
    }
}

fn strip_zeros(number: &str) -> String {
    if !number.contains('.') {
        return number.into();
    }
    number.trim_end_matches('0').trim_end_matches('.').into()
}

/// strtol's value of `text` in base 10 on a `long` of `bits` bits, and how many bytes it read (0: no number).
pub fn strtol(text: &[u8], bits: u32) -> (i64, usize) {
    let (negative, digits, start, end) = scan(text);
    if end == start {
        return (0, 0);
    }
    let max = (1i128 << (bits - 1)) - 1;
    let magnitude = digits.min(max + 1);
    let value = if negative { -magnitude } else { magnitude.min(max) };
    (value.clamp(-max - 1, max) as i64, end)
}

/// strtoul's value of `text` in base 10 on an `unsigned long` of `bits` bits, how many bytes it read, and whether
/// it overflowed (errno ERANGE). A minus sign negates the value modulo 2^bits, as C does.
pub fn strtoul(text: &[u8], bits: u32) -> (u64, usize, bool) {
    let (negative, digits, start, end) = scan(text);
    if end == start {
        return (0, 0, false);
    }
    let max = (1u128 << bits) - 1;
    if digits as u128 > max {
        return (max as u64, end, true);
    }
    let value = digits as u128;
    let value = if negative { (max + 1 - value) & max } else { value };
    (value as u64, end, false)
}

/// Leading white space and a sign skipped: whether negative, the digits' value (capped), and where the digits start
/// and end.
fn scan(text: &[u8]) -> (bool, i128, usize, usize) {
    let mut at = 0;
    while at < text.len() && matches!(text[at], b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) {
        at += 1;
    }
    let mut negative = false;
    if at < text.len() && (text[at] == b'+' || text[at] == b'-') {
        negative = text[at] == b'-';
        at += 1;
    }
    let start = at;
    let mut value: i128 = 0;
    while at < text.len() && text[at].is_ascii_digit() {
        value = (value * 10 + (text[at] - b'0') as i128).min(1 << 100);
        at += 1;
    }
    (negative, value, start, at)
}

/// glibc's strerror text of `errno`, for the errors the tools can meet; "Unknown error N" for the others, as glibc.
pub fn strerror(errno: i32) -> String {
    let text = match errno {
        0 => "Success",
        1 => "Operation not permitted",
        2 => "No such file or directory",
        3 => "No such process",
        4 => "Interrupted system call",
        5 => "Input/output error",
        6 => "No such device or address",
        7 => "Argument list too long",
        8 => "Exec format error",
        9 => "Bad file descriptor",
        10 => "No child processes",
        11 => "Resource temporarily unavailable",
        12 => "Cannot allocate memory",
        13 => "Permission denied",
        14 => "Bad address",
        16 => "Device or resource busy",
        17 => "File exists",
        19 => "No such device",
        20 => "Not a directory",
        21 => "Is a directory",
        22 => "Invalid argument",
        23 => "Too many open files in system",
        24 => "Too many open files",
        26 => "Text file busy",
        27 => "File too large",
        28 => "No space left on device",
        30 => "Read-only file system",
        31 => "Too many links",
        32 => "Broken pipe",
        36 => "File name too long",
        40 => "Too many levels of symbolic links",
        88 => "Socket operation on non-socket",
        91 => "Protocol wrong type for socket",
        97 => "Address family not supported by protocol",
        98 => "Address already in use",
        104 => "Connection reset by peer",
        107 => "Transport endpoint is not connected",
        110 => "Connection timed out",
        111 => "Connection refused",
        122 => "Disk quota exceeded",
        _ => return format!("Unknown error {errno}"),
    };
    text.into()
}
