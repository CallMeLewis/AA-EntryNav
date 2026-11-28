// Copyright (c) 2026 CallMeLewis
//! The Alps link of --bluetooth and --monitor: platform/alps_spp.cpp's AlpsSppLink in monitor mode. A private
//! connection to the system bus, the service's unique name, three match rules, and a thread that reads the bus and
//! logs every signal of the service. Only Introspect and configuration reads are ever sent.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use aa_dbus::{Connection, Received};
use evo_check::alps::{introspect_request, log_line, match_rules, read_configuration_request, AlpsMonitor, SERVICE};

/// The system bus, as libdbus finds it.
pub fn system_bus_address() -> String {
    std::env::var("DBUS_SYSTEM_BUS_ADDRESS").unwrap_or_else(|_| "unix:path=/var/run/dbus/system_bus_socket".into())
}

/// The monotonic clock in seconds, the service log's clock.
pub fn up_seconds() -> f64 {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: a valid timespec to fill.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut t) };
    t.tv_sec as f64 + t.tv_nsec as f64 / 1e9
}

/// One line of the service's log on stdout, written at once.
pub fn elog(text: &str) {
    crate::program::out(&log_line(up_seconds(), "SPP", text));
}

struct Shared {
    conn: Mutex<Connection>,
    state: Mutex<AlpsMonitor>,
    changed: Condvar,
    running: AtomicBool,
}

pub struct AlpsLink {
    shared: Arc<Shared>,
    dispatch: Option<JoinHandle<()>>,
}

fn dispatch(shared: Arc<Shared>) {
    while shared.running.load(Ordering::SeqCst) {
        let received = shared
            .conn
            .lock()
            .expect("the connection")
            .receive(Duration::from_millis(50));
        match received {
            Received::Message(m) => {
                let mut state = shared.state.lock().expect("the link's state");
                for line in state.on_message(&m) {
                    elog(&line);
                }
                if !state.active {
                    shared.running.store(false, Ordering::SeqCst);
                }
                shared.changed.notify_all();
            }
            Received::Nothing => {}
            Received::Closed => {
                shared.state.lock().expect("the link's state").active = false;
                shared.running.store(false, Ordering::SeqCst);
                shared.changed.notify_all();
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(1)); // lets the calls of the main thread take the connection
    }
}

impl AlpsLink {
    /// Connects, finds the service, subscribes and starts reading. None, with the C++'s log line, when it cannot.
    pub fn start(instance: u8) -> Option<AlpsLink> {
        let mut conn = match Connection::open_bus(&system_bus_address()) {
            Ok(c) => c,
            Err(e) => {
                elog(&format!("system bus unreachable: {}", e.message));
                return None;
            }
        };
        let owner = conn
            .ask_bus("GetNameOwner", Some(SERVICE), 2000)
            .ok()
            .and_then(|r| r.first_text().map(str::to_owned))
            .unwrap_or_default();
        if owner.is_empty() {
            elog("Alps service missing");
            return None;
        }
        for rule in match_rules() {
            conn.add_match(&rule).ok()?;
        }
        let shared = Arc::new(Shared {
            conn: Mutex::new(conn),
            state: Mutex::new(AlpsMonitor::new(&owner, instance)),
            changed: Condvar::new(),
            running: AtomicBool::new(true),
        });
        let thread = {
            let shared = Arc::clone(&shared);
            std::thread::spawn(move || dispatch(shared))
        };
        Some(AlpsLink {
            shared,
            dispatch: Some(thread),
        })
    }

    pub fn running(&self) -> bool {
        self.shared.state.lock().expect("the link's state").active
    }

    /// The introspection XML of one of the service's objects, or "" when there is none.
    pub fn introspect(&self, path: &str) -> String {
        let owner = {
            let state = self.shared.state.lock().expect("the link's state");
            if !state.active || !valid_path(path) {
                return String::new();
            }
            state.owner.clone()
        };
        let reply = self
            .shared
            .conn
            .lock()
            .expect("the connection")
            .call(introspect_request(&owner, path), 2000);
        reply
            .ok()
            .and_then(|r| r.first_text().map(str::to_owned))
            .unwrap_or_default()
    }

    /// BtApplReadConfigurationReq, then its BtApplReadConfigurationCfm: the value, or None.
    pub fn read_configuration(&self, instance: u8, id: u16, timeout_ms: u64) -> Option<Vec<u8>> {
        let mut state = self.shared.state.lock().expect("the link's state");
        if !state.active || state.poisoned || timeout_ms == 0 {
            return None;
        }
        state.waiting = Some((instance, id));
        state.status = -1;
        state.data.clear();
        let mut request = read_configuration_request(&state.owner, instance, id);
        if self
            .shared
            .conn
            .lock()
            .expect("the connection")
            .send(&mut request)
            .is_err()
        {
            state.waiting = None;
            return None;
        }
        let until = Instant::now() + Duration::from_millis(timeout_ms);
        while state.active && state.waiting.is_some() {
            let left = until.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            state = self
                .shared
                .changed
                .wait_timeout(state, left)
                .expect("the link's state")
                .0;
        }
        // The confirmation carries no transaction serial: a timed-out read is never retried.
        if state.waiting.is_some() {
            state.poisoned = true;
            state.waiting = None;
            return None;
        }
        if !state.active || state.status != 0 {
            if state.active {
                elog(&format!(
                    "configuration read of 0x{id:04x} on instance {instance} refused (status {})",
                    state.status
                ));
            }
            return None;
        }
        Some(state.data.clone())
    }

    pub fn stop(&mut self) {
        self.shared.running.store(false, Ordering::SeqCst);
        self.shared.state.lock().expect("the link's state").active = false;
        if let Some(thread) = self.dispatch.take() {
            let _ = thread.join();
        }
    }
}

impl Drop for AlpsLink {
    fn drop(&mut self) {
        self.stop();
    }
}

/// An object path as libdbus accepts it: /, or /-separated elements of [A-Za-z0-9_].
pub fn valid_path(path: &str) -> bool {
    if path == "/" {
        return true;
    }
    path.starts_with('/')
        && path[1..]
            .split('/')
            .all(|e| !e.is_empty() && e.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_'))
}
