// Copyright (c) 2026 CallMeLewis
// The std checks of evo-rsprobe on this PC, and the lines it prints. The car's libraries are checked only by the
// program itself, under QEMU with the copy of the car's system (tests/run_qemu.py) and in the car.

use evo_rsprobe::{line, std_checks, verdict, Outcome, CAR_LIBRARIES};

#[test]
fn every_std_check_passes_here() {
    let outcomes = std_checks();
    for outcome in &outcomes {
        assert!(outcome.result.is_ok(), "{}", line(outcome));
        assert!(line(outcome).ends_with(", ok"), "{}", line(outcome));
    }
    let (last, passed) = verdict(&outcomes);
    assert!(passed);
    assert_eq!(last, format!("PASS evo-rsprobe: {} checks", outcomes.len()));
}

#[test]
fn a_failure_is_named_and_fails_the_run() {
    let outcomes = vec![
        Outcome {
            name: "thread",
            result: Ok("spawn".into()),
        },
        Outcome {
            name: "libxsvideo.so.4",
            result: Err("dlopen: not found".into()),
        },
    ];
    assert_eq!(line(&outcomes[1]), "libxsvideo.so.4: FAILED, dlopen: not found");
    let (last, passed) = verdict(&outcomes);
    assert!(!passed);
    assert_eq!(last, "FAIL evo-rsprobe: 1 of 2 checks failed: libxsvideo.so.4");
}

#[test]
fn every_car_library_of_the_service_is_listed() {
    let names: Vec<&str> = CAR_LIBRARIES.iter().map(|(name, _)| *name).collect();
    for library in [
        "libxsvideo.so.4",
        "libssl.so.1.0.0",
        "libdbus-1.so.3",
        "libvmostclient.so.1",
        "libasound.so.2",
    ] {
        assert!(names.contains(&library), "{library}");
    }
    assert!(CAR_LIBRARIES.iter().all(|(_, symbols)| !symbols.is_empty()));
}
