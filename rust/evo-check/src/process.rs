// Copyright (c) 2026 CallMeLewis
//! Runs a command without a shell, with bounded time and output: platform/evo_process.cpp's runCommand.
//!
//! One difference is forced on the port. The C++ runs on the car's glibc 2.15, whose posix_spawnp starts the child
//! before it executes the program: a missing or refused program shows as a child that exited with status 127, and
//! no spawn error. musl's posix_spawnp reports the error instead. So an error from it is reported here as glibc 2.15
//! reports it, and both print `code=127 ... spawn_error=0`.

use std::ffi::CString;
use std::time::Instant;

use evo_check::CommandReport;

const MAX_OUTPUT: usize = 65536;
const SPAWN_ERROR: i32 = 127;

extern "C" {
    static environ: *const *mut libc::c_char;
}

fn mono_ms(start: Instant) -> u64 {
    start.elapsed().as_millis() as u64
}

/// Runs `executable` (looked up in PATH, as posix_spawnp does) with `args`, its output and errors captured, for at
/// most `timeout_ms`. Returns whether it exited 0 within the time with all of its output, and the report.
pub fn run_command(executable: &str, args: &[&str], timeout_ms: u64) -> (bool, CommandReport) {
    let mut report = CommandReport::default();
    if executable.is_empty() || timeout_ms == 0 {
        report.spawn_error = libc::EINVAL;
        return (false, report);
    }
    let mut fds = [0 as libc::c_int; 2];
    // SAFETY: plain C calls on descriptors this function owns; every path closes them.
    unsafe {
        if libc::pipe2(fds.as_mut_ptr(), libc::O_CLOEXEC) != 0 {
            report.spawn_error = *libc::__errno_location();
            return (false, report);
        }
        if libc::fcntl(fds[0], libc::F_SETFL, libc::O_NONBLOCK) < 0 {
            report.spawn_error = *libc::__errno_location();
            libc::close(fds[0]);
            libc::close(fds[1]);
            return (false, report);
        }
    }
    let strings: Vec<CString> = std::iter::once(executable)
        .chain(args.iter().copied())
        .map(|s| CString::new(s).unwrap_or_default())
        .collect();
    let mut argv: Vec<*mut libc::c_char> = strings.iter().map(|s| s.as_ptr() as *mut libc::c_char).collect();
    argv.push(std::ptr::null_mut());
    let mut pid: libc::pid_t = -1;
    // SAFETY: the file actions are initialised before use and destroyed after; argv and environ end with null.
    let rc = unsafe {
        let mut actions: libc::posix_spawn_file_actions_t = std::mem::zeroed();
        let init = libc::posix_spawn_file_actions_init(&mut actions);
        if init != 0 {
            report.spawn_error = init;
            libc::close(fds[0]);
            libc::close(fds[1]);
            return (false, report);
        }
        let configured = libc::posix_spawn_file_actions_adddup2(&mut actions, fds[1], 1) == 0
            && libc::posix_spawn_file_actions_adddup2(&mut actions, fds[1], 2) == 0
            && libc::posix_spawn_file_actions_addclose(&mut actions, fds[0]) == 0
            && libc::posix_spawn_file_actions_addclose(&mut actions, fds[1]) == 0;
        let rc = if configured {
            libc::posix_spawnp(
                &mut pid,
                strings[0].as_ptr(),
                &actions,
                std::ptr::null(),
                argv.as_ptr(),
                environ,
            )
        } else {
            libc::EINVAL
        };
        libc::posix_spawn_file_actions_destroy(&mut actions);
        libc::close(fds[1]);
        rc
    };
    if rc != 0 {
        // SAFETY: closing this function's own descriptor.
        unsafe { libc::close(fds[0]) };
        if configured_error(rc) {
            report.spawn_error = rc;
        } else {
            report.exit_code = SPAWN_ERROR; // as glibc 2.15's child that could not execute the program
        }
        return (false, report);
    }
    let mut output: Vec<u8> = Vec::new();
    let (mut ended, mut eof, mut ok) = (false, false, true);
    let begin = Instant::now();
    let mut status: libc::c_int = 0;
    while (!ended || !eof) && mono_ms(begin) < timeout_ms && ok {
        let mut buf = [0u8; 1024];
        // SAFETY: poll, read and waitpid on this function's descriptor and child.
        let n = unsafe {
            if !eof {
                let mut reader = libc::pollfd {
                    fd: fds[0],
                    events: libc::POLLIN,
                    revents: 0,
                };
                libc::poll(&mut reader, 1, 10);
                libc::read(fds[0], buf.as_mut_ptr().cast(), buf.len())
            } else {
                libc::usleep(10000);
                0
            }
        };
        if n > 0 {
            output.extend_from_slice(&buf[..n as usize]);
            if output.len() > MAX_OUTPUT {
                output.truncate(MAX_OUTPUT);
                ok = false;
                report.truncated = true;
            }
        } else if n == 0 {
            eof = true;
        } else {
            // SAFETY: this thread's errno.
            let errno = unsafe { *libc::__errno_location() };
            if errno != libc::EAGAIN && errno != libc::EWOULDBLOCK && errno != libc::EINTR {
                ok = false;
            }
        }
        if !ended {
            // SAFETY: waitpid on this function's child.
            let w = unsafe { libc::waitpid(pid, &mut status, libc::WNOHANG) };
            // SAFETY: this thread's errno.
            if w == pid {
                ended = true;
            } else if w < 0 && unsafe { *libc::__errno_location() } != libc::EINTR {
                ended = true;
                ok = false;
            }
        }
    }
    // SAFETY: closing this function's own descriptor.
    unsafe { libc::close(fds[0]) };
    report.text = output;
    report.timed_out = (!ended || !eof) && mono_ms(begin) >= timeout_ms;
    if ended && libc::WIFEXITED(status) {
        report.exit_code = libc::WEXITSTATUS(status);
    }
    if ended && libc::WIFSIGNALED(status) {
        report.signal = libc::WTERMSIG(status);
    }
    if !ended {
        // SAFETY: the child this function started, reaped on every path.
        unsafe {
            libc::kill(pid, libc::SIGKILL);
            while libc::waitpid(pid, &mut status, 0) < 0 && *libc::__errno_location() == libc::EINTR {}
        }
        ok = false;
    }
    let ok = ok && eof && libc::WIFEXITED(status) && libc::WEXITSTATUS(status) == 0;
    (ok, report)
}

/// Errors glibc 2.15's posix_spawnp gives itself, before any child exists: the rest come from executing the program,
/// which glibc's child reports by exiting with status 127.
fn configured_error(rc: i32) -> bool {
    matches!(rc, libc::EINVAL | libc::ENOMEM | libc::EAGAIN)
}
