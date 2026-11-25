// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Control channel between the iDrive app and the Android Auto service (see evo_control.hpp).
#include "evo_control.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "control_address.hpp"
#include "evo_log.hpp"
#include "tmp_file.hpp"

namespace evo {

bool parseControlCommand(const char* data, size_t len, ControlCommand& out) {
    if (!data || !len || len > 64) {
        return false;
    }
    char buf[65];
    memcpy(buf, data, len);
    buf[len] = 0;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = 0;
    }
    char verb[16] = "";
    int arg = 0;
    char extra[8] = "";
    int n = sscanf(buf, "%15s %d %7s", verb, &arg, extra);
    if (n < 1 || n > 2) {
        return false;
    }
    static const char* const known[] = {"ping",  "show", "hide",        "quit",      "gear",
                                        "await", "pdc",  "daynight",    "drivemode", "call",
                                        "popup", "skip", "clusterskip", "carsource", "alive"};
    bool ok = false;
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (!strcmp(verb, known[i])) {
            ok = true;
        }
    }
    if (!ok) {
        return false;
    }
    const bool needsArg = !strcmp(verb, "gear") || !strcmp(verb, "pdc") || !strcmp(verb, "daynight") ||
                          !strcmp(verb, "call") || !strcmp(verb, "popup") || !strcmp(verb, "skip") ||
                          !strcmp(verb, "clusterskip") || !strcmp(verb, "carsource") || !strcmp(verb, "alive");
    if (needsArg != (n == 2) && strcmp(verb, "drivemode")) {
        return false;
    }
    out.verb = verb;
    out.arg = arg;
    out.hasArg = n == 2;
    return true;
}

EvoControl::EvoControl()
    : fd_(-1),
      started_(false),
      running_(false),
      lastWriteMs_(0),
      cb_(nullptr),
      user_(nullptr),
      ownUid_(0),
      refused_(0),
      noCredentials_(false) {}
EvoControl::~EvoControl() {
    stop();
}

bool EvoControl::start(const char* name, const char* stateFile, Callback cb, void* user) {
    if (started_) {
        return true;
    }
    fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        ELOG("Control", "no control channel: socket: %s", strerror(errno));
        return false;
    }
    sockaddr_un a;
    const socklen_t length = controlAddress(name, a);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&a), length) != 0) {
        // Only a program that asks for this very name can hold it: "in use" is one of our services, still running.
        const int e = errno;
        ELOG("Control", "control channel \"%s\" not available (%s)%s", name, strerror(e),
             e == EADDRINUSE ? ": a service already runs" : "");
        close(fd_);
        fd_ = -1;
        return false;
    }
    // The kernel attaches the sender's credentials to each command (run): see controlSenderAllowed.
    const int on = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0) {
        ELOG("Control", "sender credentials unavailable (%s): commands are not checked", strerror(errno));
    }
    ownUid_ = geteuid();
    refused_ = 0;
    noCredentials_ = false;
    stateFile_ = stateFile ? stateFile : "";
    cb_ = cb;
    user_ = user;
    running_ = true;
    if (pthread_create(&thread_, nullptr, threadMain, this) != 0) {
        running_ = false;
        close(fd_);
        fd_ = -1;
        return false;
    }
    started_ = true;
    ELOG("Control", "iDrive app commands on the control channel \"%s\"; state in %s", name, stateFile_.c_str());
    return true;
}

void EvoControl::stop() {
    if (!started_) {
        return;
    }
    running_ = false;
    pthread_join(thread_, nullptr);
    started_ = false;
    close(fd_);
    fd_ = -1;
    if (!stateFile_.empty()) {
        unlink(stateFile_.c_str());
    }
}

void EvoControl::writeState(const char* state, int overlay) {
    if (stateFile_.empty()) {
        return;
    }
    char line[64];
    snprintf(line, sizeof(line), "%s %d %d\n", state, static_cast<int>(getpid()), overlay);
    const uint32_t now = monoMs();
    if (last_ == line && now - lastWriteMs_ < 5000) {
        return;
    }
    std::string tmp = stateFile_ + ".tmp";
    int fd = replaceTmpFile(tmp.c_str(), 0644);  // never through a link someone left in /tmp (tmp_file.hpp)
    if (fd < 0) {
        return;
    }
    ssize_t w = write(fd, line, strlen(line));
    (void)w;
    close(fd);
    if (rename(tmp.c_str(), stateFile_.c_str()) == 0) {
        if (last_ != line) {
            ELOG("Control", "state -> %s", line);
        }
        last_ = line;
        lastWriteMs_ = now;
    }
}

void* EvoControl::threadMain(void* self) {
    static_cast<EvoControl*>(self)->run();
    return nullptr;
}

void EvoControl::run() {
    char buf[96];
    while (running_) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 200) <= 0) {
            continue;
        }
        sockaddr_un from;
        memset(&from, 0, sizeof(from));
        union {
            cmsghdr align;
            char space[CMSG_SPACE(sizeof(ucred))];
        } control;
        iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_name = &from;
        m.msg_namelen = sizeof(from);
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = control.space;
        m.msg_controllen = sizeof(control.space);
        const ssize_t n = recvmsg(fd_, &m, MSG_DONTWAIT);
        if (n <= 0) {
            continue;
        }
        bool credentials = false;
        ucred sender;
        memset(&sender, 0, sizeof(sender));
        for (cmsghdr* h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
            if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_CREDENTIALS &&
                h->cmsg_len >= CMSG_LEN(sizeof(ucred))) {
                memcpy(&sender, CMSG_DATA(h), sizeof(sender));
                credentials = true;
            }
        }
        if (credentials && !controlSenderAllowed(sender.uid, ownUid_)) {
            if (++refused_ <= 5) {
                ELOG("Control", "command from pid %d (user %u) refused: only root or user %u",
                     static_cast<int>(sender.pid), static_cast<unsigned>(sender.uid), static_cast<unsigned>(ownUid_));
            }
            continue;
        }
        if (!credentials && !noCredentials_) {
            noCredentials_ = true;  // never seen on Linux with SO_PASSCRED; said once, then accepted as before
            ELOG("Control", "a command came without its sender's credentials: accepted unchecked");
        }
        ControlCommand c;
        if (!parseControlCommand(buf, static_cast<size_t>(n), c)) {
            ELOG("Control", "command ignored (%d B)", (int)n);
            continue;
        }
        if (c.verb == "ping") {
            // A sender with no address of its own cannot be answered, and a sender that does not read its replies is
            // never waited on.
            if (m.msg_namelen > sizeof(sa_family_t) && m.msg_namelen <= sizeof(from)) {
                sendto(fd_, "pong", 4, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), m.msg_namelen);
            }
            continue;
        }
        if (c.hasArg) {
            ELOG("Control", "app command: %s %d", c.verb.c_str(), c.arg);
        } else {
            ELOG("Control", "app command: %s", c.verb.c_str());
        }
        if (cb_) {
            cb_(c, user_);
        }
    }
}

}  // namespace evo
