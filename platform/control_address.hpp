// Copyright (c) 2026 CallMeLewis
// The control channel between the iDrive app and the Android Auto service: its address, and the app's two calls.
//
// A Unix datagram socket in Linux's abstract namespace: a name, with no file and no port. The channel was UDP port
// 52790 on 127.0.0.1 before. That port lies inside the range the head unit hands out to any program that asks for a
// free one (net.ipv4.ip_local_port_range), and on one start of a car another program was given it: the service, finding
// it taken, concluded that a service was already running, and Android Auto did not start for the whole trip (seen by
// the upstream project on its reference car). An abstract name is only ever taken by a program that asks for that very
// name, and the kernel frees it the moment that program ends, with no file left behind.
//
// Unlike UDP, a Unix datagram socket makes its sender wait when the receiver's queue is full (net.unix.max_dgram_qlen,
// 10 on most systems). The app's calls run on iDrive's own threads and must never wait on the service, so every send
// here is MSG_DONTWAIT: a full queue fails the send at once instead.
#ifndef AA_ENTRYNAV_PLATFORM_CONTROL_ADDRESS_HPP
#define AA_ENTRYNAV_PLATFORM_CONTROL_ADDRESS_HPP

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace evo {

// Fills the abstract address of a name and returns its length. A name too long for the address is cut.
inline socklen_t controlAddress(const char* name, struct sockaddr_un& out) {
    memset(&out, 0, sizeof(out));
    out.sun_family = AF_UNIX;
    size_t n = name ? strlen(name) : 0;
    if (n > sizeof(out.sun_path) - 1) {
        n = sizeof(out.sun_path) - 1;
    }
    if (n) {
        memcpy(out.sun_path + 1, name, n);  // sun_path[0] stays 0: the abstract namespace
    }
    return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

// The name has no file permissions to keep other programs out, so the service checks who sent each command, from the
// credentials the kernel attaches: root, or the user the service runs as (the iDrive app starts it, so they match).
inline bool controlSenderAllowed(uid_t sender, uid_t own) {
    return sender == 0 || sender == own;
}

// ---- the iDrive app's side: neither call ever waits on the service ------------------------------------------------

// Sends one command to the service holding the name. Returns 0 when sent, otherwise the reason: ECONNREFUSED when no
// service holds the name (the kernel refuses at once), EAGAIN when the service has stopped reading and its queue is
// full.
inline int sendControl(const char* name, const char* message) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return errno ? errno : EIO;
    }
    struct sockaddr_un to;
    const socklen_t length = controlAddress(name, to);
    const size_t size = strlen(message);
    const ssize_t sent = sendto(fd, message, size, MSG_DONTWAIT, reinterpret_cast<struct sockaddr*>(&to), length);
    const int error = sent == static_cast<ssize_t>(size) ? 0 : (sent < 0 && errno ? errno : EIO);
    close(fd);
    return error;
}

// True when the service answers "pong" to a "ping" within waitMs. With no service on the name, false at once.
inline bool controlAnswers(const char* name, int waitMs) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    // An address of its own, picked by the kernel (autobind: an address of the family alone), so the reply can come
    // back to this socket.
    struct sockaddr_un own;
    memset(&own, 0, sizeof(own));
    own.sun_family = AF_UNIX;
    struct sockaddr_un to;
    const socklen_t length = controlAddress(name, to);
    bool answered = false;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&own), sizeof(sa_family_t)) == 0 &&
        connect(fd, reinterpret_cast<struct sockaddr*>(&to), length) == 0 && send(fd, "ping", 4, MSG_DONTWAIT) == 4) {
        struct pollfd wait;
        wait.fd = fd;
        wait.events = POLLIN;
        wait.revents = 0;
        if (poll(&wait, 1, waitMs) > 0) {
            char reply[8];
            const ssize_t n = recv(fd, reply, sizeof(reply), MSG_DONTWAIT);
            answered = n == 4 && !memcmp(reply, "pong", 4);
        }
    }
    close(fd);
    return answered;
}

}  // namespace evo

#endif
