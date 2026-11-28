// Copyright (c) 2026 CallMeLewis
//! Plays the D-Bus corpus (tests/fixtures/dbus/messages.bin) on a private bus while the C++ and the Rust evo-ric
//! both watch it, for tests/run_qemu.py's side by side check (docs/rust-port.md, step 3). Never on the car's bus.
//!
//!   ric_play ADDRESS MESSAGES.bin
//!
//! It owns the three RIC services' names on one connection and prints `ready`. After a line on its input it sends
//! every call and signal of the corpus from a second connection, with the same path, interface, member and
//! arguments: a call to a well-known name goes to that name, one to a unique name goes to the services' connection,
//! which answers each call with the arguments (or error) of the corpus's next answer. Then it prints
//! `played N messages, R RIC calls`: both recorders must record R calls. With RIC_PLAY_LOG set it lists each call it
//! sends on its error output.

#[cfg(unix)]
fn main() -> std::process::ExitCode {
    use aa_dbus::{Connection, Message, Received, MESSAGE_ERROR, METHOD_CALL, METHOD_RETURN, SIGNAL};
    use std::time::{Duration, Instant};

    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: ric_play ADDRESS MESSAGES.bin");
        return std::process::ExitCode::from(2);
    }
    let bytes = std::fs::read(&args[2]).expect("the corpus");
    let mut corpus = Vec::new();
    let mut at = 0;
    while at < bytes.len() {
        let (m, used) = Message::parse(&bytes[at..])
            .expect("a valid message")
            .expect("a whole message");
        corpus.push(m);
        at += used;
    }
    let mut server = Connection::open_bus(&args[1]).expect("the services' connection");
    for name in evo_ric::SERVERS {
        server
            .request_name(name, aa_dbus::NAME_FLAG_DO_NOT_QUEUE)
            .expect("a name");
    }
    let mut caller = Connection::open_bus(&args[1]).expect("the caller's connection");
    println!("ready");
    let mut line = String::new();
    let _ = std::io::stdin().read_line(&mut line);
    let mut answers = corpus
        .iter()
        .filter(|m| m.kind == METHOD_RETURN || m.kind == MESSAGE_ERROR)
        .cycle();
    let (mut played, mut ric_calls) = (0, 0);
    for m in &corpus {
        let mut out = match m.kind {
            METHOD_CALL => {
                let destination = match m.destination() {
                    Some(d) if d.starts_with(':') => Some(server.unique_name().to_string()),
                    other => other.map(str::to_owned),
                };
                let mut call = Message::method_call(
                    destination.as_deref(),
                    m.path().unwrap_or("/"),
                    m.interface(),
                    m.member().unwrap_or("x"),
                );
                for v in &m.body {
                    call.append(v.clone());
                }
                call
            }
            SIGNAL => {
                let mut signal = Message::signal(
                    m.path().unwrap_or("/"),
                    m.interface().unwrap_or("x.y"),
                    m.member().unwrap_or("x"),
                );
                for v in &m.body {
                    signal.append(v.clone());
                }
                let _ = server.send(&mut signal);
                played += 1;
                continue;
            }
            _ => continue,
        };
        // A call with no destination reaches no eavesdropper on the car's dbus-daemon 1.6: not counted.
        if out.interface() == Some(evo_ric::IFACE) && out.destination().is_some() {
            ric_calls += 1;
        }
        let to_bus = out.destination() == Some(aa_dbus::BUS_NAME);
        let answered = out.destination().is_some() && !to_bus;
        out.set_no_reply(!answered && !to_bus);
        let serial = caller.send(&mut out).expect("sent");
        if std::env::var_os("RIC_PLAY_LOG").is_some() {
            eprintln!("sent s={serial} {:?} {:?}", out.destination(), out.member());
        }
        played += 1;
        if !answered {
            if to_bus {
                let _ = caller.wait_reply(serial, 2000);
            }
            continue;
        }
        // The services' connection answers the call with the corpus's next answer.
        let begin = Instant::now();
        while begin.elapsed() < Duration::from_secs(5) {
            if let Received::Message(call) = server.receive(Duration::from_millis(200)) {
                if call.kind != METHOD_CALL {
                    continue;
                }
                let model = answers.next().expect("the corpus has answers");
                let mut reply = if model.kind == METHOD_RETURN {
                    Message::method_return(&call)
                } else {
                    Message::error(&call, model.error_name().unwrap_or("BMW.Error"), None)
                };
                for v in &model.body {
                    reply.append(v.clone());
                }
                let _ = server.send(&mut reply);
                break;
            }
        }
        let _ = caller.wait_reply(serial, 5000);
    }
    // Every message out before the watchers are stopped.
    let _ = caller.ask_bus("GetId", None, 2000);
    let _ = server.ask_bus("GetId", None, 2000);
    println!("played {played} messages, {ric_calls} RIC calls");
    std::process::ExitCode::SUCCESS
}

#[cfg(not(unix))]
fn main() {}
