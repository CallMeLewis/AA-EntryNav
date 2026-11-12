// Copyright (c) 2026 CallMeLewis
// One USB device as Android Auto over a cable needs it: control requests, an interface taken (also from whoever
// holds it), and bulk transfers that never lose bytes.
//
// The car's kernel gives programs USB devices through usbfs (/dev/bus/usb/BBB/DDD). Bulk data goes as queued
// transfers that are collected when done, never as calls with a time limit: a timed-out call can drop what had
// already arrived. UsbIo is the interface the rest of the code uses; UsbfsIo is the car's (and any Linux PC's);
// the PC tests have their own.
#ifndef AA_ENTRYNAV_PLATFORM_USB_IO_HPP
#define AA_ENTRYNAV_PLATFORM_USB_IO_HPP

#include <stdint.h>
#include <stdlib.h>

namespace evo {

// One bulk transfer. The caller owns data and keeps both alive until the transfer comes back from reap().
struct UsbTransfer {
    uint8_t endpoint;  // bit 7 set: from the device
    uint8_t* data;
    int length;
    int done;         // bytes moved, once reaped
    int status;       // 0, or a negative errno, once reaped
    bool zeroPacket;  // to the device: an empty packet after data that fills its last packet exactly
    void* impl;       // the UsbIo's own record of the transfer, from malloc(), or null
    UsbTransfer() : endpoint(0), data(nullptr), length(0), done(0), status(0), zeroPacket(false), impl(nullptr) {}
    ~UsbTransfer() { free(impl); }

private:
    UsbTransfer(const UsbTransfer&);  // not copied: the kernel holds its address while it is queued
    UsbTransfer& operator=(const UsbTransfer&);
};

class UsbIo {
public:
    virtual ~UsbIo() {}
    // A control request on endpoint 0. Returns the bytes moved, or a negative errno.
    virtual int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length,
                        unsigned timeoutMs) = 0;
    // 0 or a negative errno, each.
    virtual int claim(unsigned interface) = 0;
    virtual int release(unsigned interface) = 0;
    virtual int detach(unsigned interface) = 0;  // from a kernel driver or another program; -ENODATA: nobody held it
    virtual int reset() = 0;                     // a port reset: the device starts over, in the mode it is in
    virtual int submit(UsbTransfer& transfer) = 0;
    virtual int cancel(UsbTransfer& transfer) = 0;  // it still comes back from reap()
    // One finished transfer: 1 and *transfer, 0 when none is ready, a negative errno when the device is gone.
    virtual int reap(UsbTransfer** transfer) = 0;
    // What poll() must wait for to know reap() has something.
    virtual int waitFd() const = 0;
    virtual short waitEvents() const = 0;
};

class UsbfsIo : public UsbIo {
public:
    UsbfsIo();
    ~UsbfsIo() override;
    // root: /dev/bus/usb on the car. False with errno set.
    bool open(unsigned bus, unsigned device, const char* root = "/dev/bus/usb");
    void close();
    bool isOpen() const { return fd_ >= 0; }

    int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length,
                unsigned timeoutMs) override;
    int claim(unsigned interface) override;
    int release(unsigned interface) override;
    int detach(unsigned interface) override;
    int reset() override;
    int submit(UsbTransfer& transfer) override;
    int cancel(UsbTransfer& transfer) override;
    int reap(UsbTransfer** transfer) override;
    int waitFd() const override { return fd_; }
    short waitEvents() const override;

private:
    int fd_;
};

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_USB_IO_HPP
