// Copyright (c) 2026 CallMeLewis
// aa-dbus's wire format against libdbus's (tests/fixtures/dbus/messages.bin, made and marshalled by libdbus in
// tests/test_ric_golden.cpp): every message reads, every little-endian one writes back to the same bytes, and the
// messages aa-dbus builds are laid out as libdbus lays them out.

use aa_dbus::{parse_address, single_types, Address, Message, Value, FIELD_SENDER, METHOD_RETURN, SIGNAL};

const MESSAGES: &[u8] = include_bytes!("../../../tests/fixtures/dbus/messages.bin");

fn messages() -> Vec<(Message, &'static [u8])> {
    let mut out = Vec::new();
    let mut at = 0;
    while at < MESSAGES.len() {
        let (m, used) = Message::parse(&MESSAGES[at..]).expect("valid").expect("whole");
        out.push((m, &MESSAGES[at..at + used]));
        at += used;
    }
    out
}

#[test]
fn every_message_reads_and_writes_back_as_libdbus_wrote_it() {
    let all = messages();
    assert_eq!(all.len(), 239);
    let mut big = 0;
    for (i, (m, bytes)) in all.iter().enumerate() {
        if m.big_endian {
            big += 1;
            // Written little-endian, it must read back as the same message.
            let again = Message::parse(&m.marshal()).expect("valid").expect("whole").0;
            assert_eq!(again.body, m.body, "message {i}");
            continue;
        }
        assert_eq!(
            m.marshal(),
            *bytes,
            "message {i} written back differs from libdbus's bytes"
        );
    }
    assert_eq!(big, 2);
}

#[test]
fn a_big_endian_message_reads_every_type() {
    let all = messages();
    let (m, _) = all
        .iter()
        .find(|(m, _)| m.big_endian && m.kind == SIGNAL)
        .expect("the big-endian signal");
    assert_eq!(m.serial, 0x01020304);
    assert_eq!(m.sender(), Some(":1.33"));
    assert_eq!(
        m.body,
        vec![
            Value::Byte(200),
            Value::Int16(-2),
            Value::Uint16(65535),
            Value::Int32(-100000),
            Value::Uint32(4000000000),
            Value::Int64(-1),
            Value::Uint64(u64::MAX),
            Value::Double(3.25),
            Value::String("Big end".into()),
            Value::ObjectPath("/a/b".into()),
            Value::Array("s".into(), vec![Value::String("x".into()), Value::String("yz".into())]),
        ]
    );
}

#[test]
fn a_call_and_its_answer_built_here_are_libdbus_s_bytes() {
    let all = messages();
    // The fixture's first two messages: the self test's call, and the service's answer, with the bus's sender.
    let mut call = Message::method_call(
        Some("BMW.RueKoIC"),
        "/BMW/RueKoIC",
        Some("BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.RICServiceGuidance"),
        "ric_setResource",
    )
    .with([
        Value::Uint32(7),
        Value::Uint32(0x03000000),
        Value::Int32(3),
        Value::Struct(vec![
            Value::Byte(9),
            Value::Variant(Box::new(Value::String("Main St".into()))),
        ]),
    ]);
    call.serial = 101;
    call.set_field(FIELD_SENDER, Some(Value::String(":1.20".into())));
    assert_eq!(call.marshal(), all[0].1);
    let mut answer = Message::method_return(&call).with([Value::Int32(0)]);
    answer.serial = 102;
    answer.set_field(FIELD_SENDER, Some(Value::String(":1.5".into())));
    assert_eq!(answer.kind, METHOD_RETURN);
    assert_eq!(answer.marshal(), all[1].1);
    // The error answer: destination, error name, reply serial, signature.
    let mut asked = Message::method_call(Some("BMW.Kombi"), "/BMW/RueKoIC", None, "ric_initialize");
    asked.serial = 41;
    asked.set_field(FIELD_SENDER, Some(Value::String(":1.21".into())));
    let mut error = Message::error(
        &asked,
        "org.freedesktop.DBus.Error.UnknownMethod",
        Some("No \"such\" method"),
    );
    error.serial = 103;
    error.set_field(FIELD_SENDER, Some(Value::String(":1.6".into())));
    assert_eq!(error.marshal(), all[2].1);
}

#[test]
fn an_incomplete_message_waits_and_a_bad_one_is_refused() {
    let all = messages();
    let bytes = all[0].1;
    assert_eq!(Message::parse(&bytes[..10]), Ok(None));
    assert_eq!(Message::parse(&bytes[..bytes.len() - 1]), Ok(None));
    let mut bad = bytes.to_vec();
    bad[0] = b'x';
    assert!(Message::parse(&bad).is_err());
    let mut version = bytes.to_vec();
    version[3] = 2;
    assert!(Message::parse(&version).is_err());
}

#[test]
fn signatures_split_into_complete_types() {
    assert_eq!(single_types("uui(yv)"), Some(vec!["u", "u", "i", "(yv)"]));
    assert_eq!(single_types("a{sv}aay"), Some(vec!["a{sv}", "aay"]));
    assert_eq!(single_types(""), Some(vec![]));
    for bad in ["(", "()", "a", "{sv}", "a{vs}", "a{s}", "z"] {
        assert_eq!(single_types(bad), None, "{bad}");
    }
}

#[test]
fn addresses_as_libdbus_reads_them() {
    assert_eq!(
        parse_address("unix:path=/tmp/session_bus_socket"),
        Ok(vec![Address::Path("/tmp/session_bus_socket".into())])
    );
    assert_eq!(
        parse_address("unix:abstract=/tmp/dbus-AbC,guid=0123;unix:path=/run/x%20y"),
        Ok(vec![
            Address::Abstract("/tmp/dbus-AbC".into()),
            Address::Path("/run/x y".into())
        ])
    );
    assert!(parse_address("tcp:host=localhost,port=1").is_err());
    assert!(parse_address("unix:guid=1").is_err());
}
