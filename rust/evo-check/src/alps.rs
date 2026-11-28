// Copyright (c) 2026 CallMeLewis
//! The car's Alps Bluetooth stack as evo-check reads it: the monitor mode of platform/alps_spp.cpp's link, which
//! logs every SPP and DM signal of the service and sends only Introspect and configuration reads.
//!
//! D-Bus interface of the Alps stack, as the BMW Bluetooth services use it: service org.alps.evolution, object
//! /org/alps/evolution/hli, interfaces org.alps.evo.SPP and org.alps.evo.DM. The writes of the C++ link (UUID
//! registration, configuration writes, SPP data) are refused in monitor mode, so the port has none of them.

use aa_dbus::{Message, Value, BUS_INTERFACE, BUS_NAME, SIGNAL};

pub const SERVICE: &str = "org.alps.evolution";
pub const PATH: &str = "/org/alps/evolution/hli";
pub const SPP: &str = "org.alps.evo.SPP";
pub const DM: &str = "org.alps.evo.DM";
/// The configuration that says whether an SPP instance carries data over D-Bus.
pub const DATA_INTERFACE: u16 = 0x6203;

/// The match rules of the link, added in this order.
pub fn match_rules() -> [String; 3] {
    [
        format!("type='signal',sender='{SERVICE}',path='{PATH}',interface='{SPP}'"),
        format!("type='signal',sender='{SERVICE}',path='{PATH}',interface='{DM}'"),
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='\
         org.alps.evolution'"
            .into(),
    ]
}

/// Introspect, to the service's unique name.
pub fn introspect_request(owner: &str, path: &str) -> Message {
    Message::method_call(
        Some(owner),
        path,
        Some("org.freedesktop.DBus.Introspectable"),
        "Introspect",
    )
}

/// BtApplReadConfigurationReq(y instance, q id); Alps answers with a signal, so no reply is expected.
pub fn read_configuration_request(owner: &str, instance: u8, id: u16) -> Message {
    let mut m = Message::method_call(Some(owner), PATH, Some(DM), "BtApplReadConfigurationReq")
        .with([Value::Byte(instance), Value::Uint16(id)]);
    m.set_no_reply(true);
    m
}

/// The link's state: whose signals it reads, and the configuration read it waits for.
pub struct AlpsMonitor {
    pub owner: String,
    pub instance: u8,
    pub active: bool,
    /// (instance, id) of the read waiting for its BtApplReadConfigurationCfm.
    pub waiting: Option<(u8, u16)>,
    pub status: i32,
    pub data: Vec<u8>,
    /// Set once a read timed out: the confirmation carries no serial, so the slot is never reused.
    pub poisoned: bool,
}

fn byte(v: Option<&Value>) -> u8 {
    match v {
        Some(Value::Byte(b)) => *b,
        _ => 0,
    }
}

fn word(v: Option<&Value>) -> u16 {
    match v {
        Some(Value::Uint16(n)) => *n,
        _ => 0,
    }
}

impl AlpsMonitor {
    pub fn new(owner: &str, instance: u8) -> AlpsMonitor {
        AlpsMonitor {
            owner: owner.into(),
            instance,
            active: true,
            waiting: None,
            status: -1,
            data: Vec::new(),
            poisoned: false,
        }
    }

    /// The link's filter on one message: the log lines it writes (tag SPP), in order.
    pub fn on_message(&mut self, m: &Message) -> Vec<String> {
        let mut log = Vec::new();
        if m.is_signal(BUS_INTERFACE, "NameOwnerChanged") && m.sender() == Some(BUS_NAME) {
            let names: Vec<&str> = m
                .body
                .iter()
                .take(3)
                .filter_map(|v| match v {
                    Value::String(s) => Some(s.as_str()),
                    _ => None,
                })
                .collect();
            if names.len() == 3 && names[0] == SERVICE && self.owner != names[2] {
                log.push("Alps service replaced or lost: the link must restart".into());
                self.active = false;
            }
            return log;
        }
        if !self.active || m.path() != Some(PATH) || m.sender() != Some(self.owner.as_str()) || m.kind != SIGNAL {
            return log;
        }
        self.signal(m, &mut log);
        log
    }

    fn signal(&mut self, m: &Message, log: &mut Vec<String>) {
        let Some(member) = m.member() else {
            return;
        };
        // Metadata only: no SPP payload or Wi-Fi passphrase in diagnostics.
        let interface = m.interface().unwrap_or("(null)");
        match (m.interface() == Some(SPP), m.body.first()) {
            (true, Some(Value::Byte(instance))) => log.push(format!(
                "observed {interface}.{member} ({}) instance={instance}",
                m.signature()
            )),
            _ => log.push(format!("observed {interface}.{member} ({})", m.signature())),
        }
        if m.interface() != Some(DM) {
            return; // SPP: in monitor mode only a UUID confirmation could matter, and none is awaited
        }
        let b = &m.body;
        if member == "BtApplReadConfigurationCfm" || member == "BtApplSetConfigurationCfm" {
            let writing = member == "BtApplSetConfigurationCfm";
            if m.signature() != if writing { "yyq" } else { "yyqqay" } {
                return;
            }
            let (status, instance, id) = (byte(b.first()), byte(b.get(1)), word(b.get(2)));
            if writing || self.waiting != Some((instance, id)) {
                return; // monitor mode never writes
            }
            self.data.clear();
            let data = b[4].as_bytes().unwrap_or_default();
            if data.len() != word(b.get(3)) as usize {
                return;
            }
            self.data = data;
            self.status = status as i32;
            self.waiting = None;
            return;
        }
        let connecting = member == "BtApplDeviceConnectInd";
        if !connecting && member != "BtApplDeviceDisconnectInd" {
            return;
        }
        if m.signature() != if connecting { "yayty" } else { "yayyty" } {
            return;
        }
        let status = byte(b.first());
        if b[1].as_bytes().map(|a| a.len()) != Some(6) {
            return;
        }
        let mut at = 2;
        let reason = if connecting {
            0
        } else {
            at += 1;
            byte(b.get(2))
        };
        let function = match b.get(at) {
            Some(Value::Uint64(f)) => *f,
            _ => 0,
        };
        let instance = byte(b.get(at + 1));
        // Every Bluetooth connection the car makes, accepts or loses, with any device. No address is written; in
        // monitor mode the link never knows the phone, so no line says whose it is.
        if connecting {
            log.push(format!(
                "{member} instance={instance} function={function} status={status}"
            ));
        } else {
            log.push(format!(
                "{member} instance={instance} function={function} status={status} reason={reason}"
            ));
        }
    }
}

/// One line of the service's log (platform/evo_log.hpp): the uptime, the tag, the text, at most 1022 bytes.
pub fn log_line(uptime: f64, tag: &str, text: &str) -> Vec<u8> {
    let mut line = format!("[{uptime:9.3}] [{tag}] {text}").into_bytes();
    line.truncate(1022);
    line.push(b'\n');
    line
}
