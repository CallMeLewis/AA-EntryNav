// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The socket <-> USB accessory bridge (see usb_bridge.hpp).
#include "usb_bridge.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace evo {

UsbBridge::UsbBridge()
    : io_(nullptr),
      ours_(-1),
      thread_(0),
      started_(false),
      running_(false),
      quit_(false),
      pendingAt_(0),
      pendingEnd_(0),
      outBusy_(false) {
    pthread_mutex_init(&mu_, nullptr);
    memset(&stats_, 0, sizeof(stats_));
}

UsbBridge::~UsbBridge() {
    stop();
    pthread_mutex_destroy(&mu_);
}

int UsbBridge::start(UsbIo* io, uint8_t in, uint8_t out) {
    stop();
    int pair[2];
    if (!io || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        return -1;
    }
    io_ = io;
    ours_ = pair[0];
    in_.endpoint = in | 0x80;
    in_.data = inBuffer_;
    in_.length = kUsbChunk;
    out_.endpoint = out & 0x7F;
    out_.data = outBuffer_;
    out_.zeroPacket = true;
    pendingAt_ = pendingEnd_ = 0;
    outBusy_ = false;
    pthread_mutex_lock(&mu_);
    memset(&stats_, 0, sizeof(stats_));
    pthread_mutex_unlock(&mu_);
    quit_ = false;
    running_ = true;
    if (pthread_create(&thread_, nullptr, threadMain, this) != 0) {
        running_ = false;
        close(pair[0]);
        close(pair[1]);
        ours_ = -1;
        return -1;
    }
    started_ = true;
    return pair[1];
}

void UsbBridge::stop() {
    if (!started_) {
        return;
    }
    quit_ = true;
    pthread_join(thread_, nullptr);
    started_ = false;
}

UsbBridge::Stats UsbBridge::stats() const {
    pthread_mutex_lock(&mu_);
    const Stats copy = stats_;
    pthread_mutex_unlock(&mu_);
    return copy;
}

void* UsbBridge::threadMain(void* self) {
    static_cast<UsbBridge*>(self)->run();
    return nullptr;
}

bool UsbBridge::flushToSession() {
    while (pendingAt_ < pendingEnd_) {
        const ssize_t n = send(ours_, inBuffer_ + pendingAt_, pendingEnd_ - pendingAt_, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            pendingAt_ += static_cast<int>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;  // the session is busy: poll() says when its socket takes more
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void UsbBridge::run() {
    int error = 0;
    bool inQueued = io_->submit(in_) == 0;
    if (!inQueued) {
        error = -EIO;
    }
    while (!quit_ && !error) {
        pollfd fds[2];
        fds[0].fd = io_->waitFd();
        fds[0].events = io_->waitEvents();
        fds[0].revents = 0;
        fds[1].fd = ours_;
        fds[1].events = static_cast<short>((outBusy_ ? 0 : POLLIN) | (pendingAt_ < pendingEnd_ ? POLLOUT : 0));
        fds[1].revents = 0;
        const int ready = poll(fds, 2, 200);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = -errno;
            break;
        }
        // Finished transfers, whatever poll() said: a fake or a kernel may report them late.
        for (;;) {
            UsbTransfer* done = nullptr;
            const int got = io_->reap(&done);
            if (got < 0) {
                error = got;  // the phone was unplugged
                break;
            }
            if (got == 0) {
                break;
            }
            if (done == &in_) {
                inQueued = false;
                if (in_.status != 0) {
                    error = in_.status;
                    break;
                }
                pendingAt_ = 0;
                pendingEnd_ = in_.done;
                pthread_mutex_lock(&mu_);
                stats_.fromPhone += static_cast<uint64_t>(in_.done);
                ++stats_.transfersIn;
                pthread_mutex_unlock(&mu_);
            } else if (done == &out_) {
                if (out_.status != 0) {
                    error = out_.status;
                    break;
                }
                pthread_mutex_lock(&mu_);
                stats_.toPhone += static_cast<uint64_t>(out_.done);
                ++stats_.transfersOut;
                pthread_mutex_unlock(&mu_);
                if (out_.done < out_.length) {  // cut short: the rest goes as a transfer of its own
                    memmove(outBuffer_, outBuffer_ + out_.done, out_.length - out_.done);
                    out_.length -= out_.done;
                    if (io_->submit(out_) != 0) {
                        error = -EIO;
                        break;
                    }
                } else {
                    outBusy_ = false;
                }
            }
        }
        if (error) {
            break;
        }
        if (!flushToSession()) {
            break;  // the session closed its socket
        }
        // The socket is not read while a transfer to the phone is pending, so its end would go unseen. A phone whose
        // Android Auto stopped may never take that transfer: the bridge would then run, and keep the cable, forever.
        if (outBusy_ && (fds[1].revents & (POLLHUP | POLLERR))) {
            break;  // the session closed its socket
        }
        if (!inQueued && pendingAt_ >= pendingEnd_) {
            in_.length = kUsbChunk;
            if (io_->submit(in_) != 0) {
                error = -EIO;
                break;
            }
            inQueued = true;
        }
        if (!outBusy_ && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = recv(ours_, outBuffer_, kUsbChunk, MSG_DONTWAIT);
            if (n > 0) {
                out_.length = static_cast<int>(n);
                if (io_->submit(out_) != 0) {
                    error = -EIO;
                    break;
                }
                outBusy_ = true;
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                break;  // the session closed its socket
            }
        }
    }
    // What is still queued is cancelled and collected, so that the buffers are no longer the kernel's.
    if (inQueued) {
        io_->cancel(in_);
    }
    if (outBusy_) {
        io_->cancel(out_);
    }
    for (int turn = 0; (inQueued || outBusy_) && turn < 50; ++turn) {
        UsbTransfer* done = nullptr;
        const int got = io_->reap(&done);
        if (got < 0) {
            break;  // the device is gone: the kernel dropped them with it
        }
        if (got == 0) {
            pollfd fd;
            fd.fd = io_->waitFd();
            fd.events = io_->waitEvents();
            fd.revents = 0;
            poll(&fd, 1, 20);
            continue;
        }
        if (done == &in_) {
            inQueued = false;
        } else if (done == &out_) {
            outBusy_ = false;
        }
    }
    pthread_mutex_lock(&mu_);
    stats_.error = error;
    pthread_mutex_unlock(&mu_);
    close(ours_);  // the session reads the end of its socket and stops
    ours_ = -1;
    running_ = false;
}

}  // namespace evo
