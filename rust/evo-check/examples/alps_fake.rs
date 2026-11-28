// Copyright (c) 2026 CallMeLewis
//! A fake Alps Bluetooth service on a private bus, for tests/run_qemu.py's side by side check of the C++ and Rust
//! evo-check (docs/rust-port.md, step 4). Never on the car's bus.
//!
//!   alps_fake ADDRESS
//!
//! It owns org.alps.evolution, prints `ready`, and answers Introspect with a fixed description and each
//! BtApplReadConfigurationReq with a BtApplReadConfigurationCfm of value 0. Each line on its input is an order:
//! `script` sends the SPP and DM signals tests/test_check_golden.cpp's fake sends, `release` gives up the name, and
//! `quit` ends it.

#[cfg(unix)]
fn main() {
    use aa_dbus::{Connection, Message, Received, Value, METHOD_CALL};
    use evo_check::alps::{DM, PATH, SERVICE, SPP};
    use std::sync::mpsc;
    use std::time::Duration;

    const XML: &str = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n\
                       \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n\
                       <node><interface name=\"org.alps.evo.SPP\"><signal name=\"BtApplSppDataInd\"/></interface>\
                       </node>";

    let address = std::env::args().nth(1).expect("usage: alps_fake ADDRESS");
    let mut c = Connection::open_bus(&address).expect("the bus");
    c.request_name(SERVICE, aa_dbus::NAME_FLAG_DO_NOT_QUEUE)
        .expect("the name org.alps.evolution");
    println!("ready");
    let (orders, inbox) = mpsc::channel();
    std::thread::spawn(move || {
        let mut line = String::new();
        while std::io::stdin().read_line(&mut line).is_ok_and(|n| n > 0) {
            let _ = orders.send(line.trim().to_string());
            line.clear();
        }
        let _ = orders.send("quit".into());
    });
    let a = Value::bytes(&[1, 2, 3, 4, 5, 6]);
    let short = Value::bytes(&[1, 2, 3, 4, 5]);
    let four = Value::bytes(&[1, 2, 3, 4]);
    let script: Vec<(&str, &str, Vec<Value>)> = vec![
        (SPP, "BtApplSppSetUuidCfm", vec![Value::Byte(0), Value::Byte(0)]),
        (
            SPP,
            "BtApplSppDataInd",
            vec![Value::Byte(1), Value::Uint16(4), four.clone()],
        ),
        (SPP, "BtApplSppDataInd", vec![Value::Byte(0), Value::Uint16(4), four]),
        (SPP, "BtApplSppDataCfm", vec![Value::Byte(0), Value::Byte(3)]),
        (SPP, "BtApplSppSendInd", vec![Value::Byte(5)]),
        (SPP, "BtApplSppDataSendFcInd", vec![Value::Byte(0), Value::Byte(1)]),
        (SPP, "BtApplOddInd", vec![Value::String("odd".into())]),
        (SPP, "BtApplEmptyInd", vec![]),
        (
            DM,
            "BtApplDeviceConnectInd",
            vec![Value::Byte(0), a.clone(), Value::Uint64(1), Value::Byte(0)],
        ),
        (
            DM,
            "BtApplDeviceConnectInd",
            vec![Value::Byte(0x80), a.clone(), Value::Uint64(2), Value::Byte(3)],
        ),
        (
            DM,
            "BtApplDeviceDisconnectInd",
            vec![
                Value::Byte(0),
                a.clone(),
                Value::Byte(2),
                Value::Uint64(1),
                Value::Byte(0),
            ],
        ),
        (
            DM,
            "BtApplDeviceConnectInd",
            vec![Value::Byte(0), short, Value::Uint64(1), Value::Byte(0)],
        ),
        (DM, "BtApplDeviceConnectInd", vec![Value::Byte(0), a]),
        (
            DM,
            "BtApplSetConfigurationCfm",
            vec![Value::Byte(0), Value::Byte(0), Value::Uint16(0x6203)],
        ),
        (
            DM,
            "BtApplReadConfigurationCfm",
            vec![
                Value::Byte(0),
                Value::Byte(5),
                Value::Uint16(0x6203),
                Value::Uint16(1),
                Value::bytes(&[1]),
            ],
        ),
        (DM, "BtApplReadConfigurationCfm", vec![Value::Byte(0), Value::Byte(0)]),
        (DM, "BtApplSomethingInd", vec![Value::Uint64(2)]),
    ];
    loop {
        if let Received::Message(m) = c.receive(Duration::from_millis(20)) {
            if m.kind == METHOD_CALL && m.member() == Some("Introspect") {
                let mut r = Message::method_return(&m).with([Value::String(XML.into())]);
                let _ = c.send(&mut r);
            } else if m.kind == METHOD_CALL && m.member() == Some("BtApplReadConfigurationReq") {
                let mut s = Message::signal(PATH, DM, "BtApplReadConfigurationCfm").with([
                    Value::Byte(0),
                    m.body.first().cloned().unwrap_or(Value::Byte(0)),
                    m.body.get(1).cloned().unwrap_or(Value::Uint16(0)),
                    Value::Uint16(1),
                    Value::bytes(&[0]),
                ]);
                let _ = c.send(&mut s);
            }
        }
        match inbox.try_recv().as_deref() {
            Ok("script") => {
                for (interface, member, args) in &script {
                    let mut s = Message::signal(PATH, interface, member).with(args.iter().cloned());
                    let _ = c.send(&mut s);
                    std::thread::sleep(Duration::from_millis(30));
                }
                println!("played");
            }
            Ok("release") => {
                let _ = c.ask_bus("ReleaseName", Some(SERVICE), 2000);
                println!("released");
            }
            Ok("quit") => return,
            _ => {}
        }
    }
}

#[cfg(not(unix))]
fn main() {}
