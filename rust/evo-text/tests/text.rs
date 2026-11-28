// Copyright (c) 2026 CallMeLewis
// printf's %g, strtol, strtoul and strerror against what glibc gives (the values were printed by the CI image's glibc
// printf and parsed by its strtol and strtoul). The D-Bus fixtures check format_g further, against the C++ itself.

use evo_text::{format_g, strerror, strtol, strtoul};

#[test]
fn percent_g_as_glibc_prints_it() {
    let cases: &[(f64, usize, &str)] = &[
        (0.0, 6, "0"),
        (-0.0, 6, "-0"),
        (1.0, 6, "1"),
        (0.1, 6, "0.1"),
        (1.5, 6, "1.5"),
        (100000.0, 6, "100000"),
        (1000000.0, 6, "1e+06"),
        (1234567.0, 6, "1.23457e+06"),
        (999999.5, 6, "1e+06"),
        (0.0001, 6, "0.0001"),
        (0.00001, 6, "1e-05"),
        (0.000123456789, 6, "0.000123457"),
        (1e100, 6, "1e+100"),
        (-2.5e-300, 6, "-2.5e-300"),
        (5e-324, 6, "4.94066e-324"),
        (f64::MAX, 6, "1.79769e+308"),
        (std::f64::consts::PI, 6, "3.14159"),
        (std::f64::consts::PI, 1, "3"),
        (std::f64::consts::PI, 0, "3"),
        (123.456, 2, "1.2e+02"),
        (0.5, 6, "0.5"),
        (2.5, 1, "2"),
        (f64::INFINITY, 6, "inf"),
        (f64::NEG_INFINITY, 6, "-inf"),
        (f64::NAN, 6, "nan"),
        (-f64::NAN, 6, "-nan"),
    ];
    for &(value, precision, expected) in cases {
        assert_eq!(
            format_g(value, precision),
            expected,
            "{value:e} with precision {precision}"
        );
    }
}

#[test]
fn strtol_as_on_a_32_bit_long() {
    assert_eq!(strtol(b"42", 32), (42, 2));
    assert_eq!(strtol(b"  -7x", 32), (-7, 4));
    assert_eq!(strtol(b"+15", 32), (15, 3));
    assert_eq!(strtol(b"abc", 32), (0, 0));
    assert_eq!(strtol(b"", 32), (0, 0));
    assert_eq!(strtol(b"-", 32), (0, 0));
    assert_eq!(strtol(b"99999999999", 32), (2147483647, 11));
    assert_eq!(strtol(b"-99999999999", 32), (-2147483648, 12));
    assert_eq!(strtol(b"99999999999", 64), (99999999999, 11));
}

#[test]
fn strtoul_as_on_a_32_bit_long() {
    assert_eq!(strtoul(b"30", 32), (30, 2, false));
    assert_eq!(strtoul(b"-1", 32), (4294967295, 2, false));
    assert_eq!(strtoul(b"4294967296", 32), (4294967295, 10, true));
    assert_eq!(strtoul(b" 7 ", 32), (7, 2, false));
    assert_eq!(strtoul(b"x", 32), (0, 0, false));
}

#[test]
fn glibc_error_texts() {
    assert_eq!(strerror(2), "No such file or directory");
    assert_eq!(strerror(13), "Permission denied");
    assert_eq!(strerror(111), "Connection refused");
    assert_eq!(strerror(9999), "Unknown error 9999");
}
