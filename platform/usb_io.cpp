// Copyright (c) 2026 CallMeLewis
// A USB device through the kernel's usbfs (see usb_io.hpp).
#include "usb_io.hpp"

#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace evo {

namespace {

int result(int rc) {
    return rc < 0 ? -errno : rc;
}

}  // namespace

UsbfsIo::UsbfsIo() : fd_(-1) {}

UsbfsIo::~UsbfsIo() {
    close();
}

bool UsbfsIo::open(unsigned bus, unsigned device, const char* root) {
    close();
    char path[96];
    snprintf(path, sizeof(path), "%s/%03u/%03u", root, bus, device);
    fd_ = ::open(path, O_RDWR | O_CLOEXEC);
    return fd_ >= 0;
}

void UsbfsIo::close() {
    if (fd_ >= 0) {
        ::close(fd_);  // the kernel cancels what is still queued and gives the interfaces back
        fd_ = -1;
    }
}

int UsbfsIo::control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length,
                     unsigned timeoutMs) {
    usbdevfs_ctrltransfer transfer;
    memset(&transfer, 0, sizeof(transfer));
    transfer.bRequestType = type;
    transfer.bRequest = request;
    transfer.wValue = value;
    transfer.wIndex = index;
    transfer.wLength = length;
    transfer.timeout = timeoutMs;
    transfer.data = data;
    return result(ioctl(fd_, USBDEVFS_CONTROL, &transfer));
}

int UsbfsIo::claim(unsigned interface) {
    return result(ioctl(fd_, USBDEVFS_CLAIMINTERFACE, &interface));
}

int UsbfsIo::release(unsigned interface) {
    return result(ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &interface));
}

int UsbfsIo::detach(unsigned interface) {
    usbdevfs_ioctl command;
    memset(&command, 0, sizeof(command));
    command.ifno = static_cast<int>(interface);
    command.ioctl_code = USBDEVFS_DISCONNECT;
    command.data = nullptr;
    return result(ioctl(fd_, USBDEVFS_IOCTL, &command));
}

int UsbfsIo::reset() {
    return result(ioctl(fd_, USBDEVFS_RESET, nullptr));
}

int UsbfsIo::submit(UsbTransfer& transfer) {
    usbdevfs_urb* urb = static_cast<usbdevfs_urb*>(transfer.impl);
    if (!urb) {
        urb = static_cast<usbdevfs_urb*>(calloc(1, sizeof(usbdevfs_urb)));
        if (!urb) {
            return -ENOMEM;
        }
        transfer.impl = urb;
    }
    memset(urb, 0, sizeof(*urb));
    urb->type = USBDEVFS_URB_TYPE_BULK;
    urb->endpoint = transfer.endpoint;
    urb->buffer = transfer.data;
    urb->buffer_length = transfer.length;
    urb->usercontext = &transfer;
#ifdef USBDEVFS_URB_ZERO_PACKET
    if (transfer.zeroPacket && !(transfer.endpoint & 0x80)) {
        urb->flags |= USBDEVFS_URB_ZERO_PACKET;
    }
#endif
    transfer.done = 0;
    transfer.status = 0;
    return result(ioctl(fd_, USBDEVFS_SUBMITURB, urb));
}

int UsbfsIo::cancel(UsbTransfer& transfer) {
    return transfer.impl ? result(ioctl(fd_, USBDEVFS_DISCARDURB, transfer.impl)) : -EINVAL;
}

int UsbfsIo::reap(UsbTransfer** transfer) {
    usbdevfs_urb* urb = nullptr;
    if (ioctl(fd_, USBDEVFS_REAPURBNDELAY, &urb) < 0) {
        return errno == EAGAIN ? 0 : -errno;
    }
    UsbTransfer* done = static_cast<UsbTransfer*>(urb->usercontext);
    done->done = urb->actual_length;
    done->status = urb->status;
    *transfer = done;
    return 1;
}

short UsbfsIo::waitEvents() const {
    return POLLOUT;  // usbfs: writable when a transfer can be collected
}

}  // namespace evo
