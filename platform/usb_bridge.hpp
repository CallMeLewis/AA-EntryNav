// Copyright (c) 2026 CallMeLewis
// Carries an Android Auto session over a USB accessory interface: the session keeps talking to a socket, as it
// does with a phone on Wi-Fi, and this bridge moves the bytes between that socket and the two bulk endpoints.
//
// One thread, nothing blocking but poll(): a transfer from the phone is queued at all times; what it brings goes to
// the socket before the next one is queued, and a transfer to the phone is queued when the socket has bytes and the
// previous one is done. Neither direction can hold up the other, so the session and the bridge cannot wait for each
// other. Transfers to the phone end with an empty packet when they fill their last packet exactly: the phone's
// accessory driver hands data to Android Auto only at the end of a transfer.
#ifndef AA_ENTRYNAV_PLATFORM_USB_BRIDGE_HPP
#define AA_ENTRYNAV_PLATFORM_USB_BRIDGE_HPP

#include <pthread.h>
#include <stdint.h>

#include <atomic>

#include "usb_io.hpp"

namespace evo {

const int kUsbChunk = 16384;  // one transfer: a multiple of the endpoint's packet size (512, or 64 at full speed)

class UsbBridge {
public:
    struct Stats {
        uint64_t fromPhone, toPhone;  // bytes
        unsigned transfersIn, transfersOut;
        int error;  // what ended it: 0 the session closed its socket, else a negative errno from the USB side
    };

    UsbBridge();
    ~UsbBridge();

    // io: an opened device whose accessory interface is claimed; it must outlive the bridge. Returns the session's
    // socket, which the caller owns, or -1. The bridge runs until that socket is closed or the phone is gone.
    int start(UsbIo* io, uint8_t in, uint8_t out);
    void stop();  // asks the thread to end and joins it; the session then reads the end of its socket
    bool running() const { return running_; }
    Stats stats() const;

private:
    static void* threadMain(void* self);
    void run();
    bool flushToSession();  // false: the session's socket is gone

    UsbIo* io_;
    int ours_;  // our end of the socket pair
    pthread_t thread_;
    bool started_;
    std::atomic<bool> running_, quit_;
    UsbTransfer in_, out_;
    uint8_t inBuffer_[kUsbChunk], outBuffer_[kUsbChunk];
    int pendingAt_, pendingEnd_;  // bytes of inBuffer_ not yet written to the socket
    bool outBusy_;
    mutable pthread_mutex_t mu_;
    Stats stats_;  // under mu_
};

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_USB_BRIDGE_HPP
