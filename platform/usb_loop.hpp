// Copyright (c) 2026 CallMeLewis
// One cable polling turn, shared by the service and the fake-phone tests. No sleep during a retry pause.
#ifndef AA_ENTRYNAV_PLATFORM_USB_LOOP_HPP
#define AA_ENTRYNAV_PLATFORM_USB_LOOP_HPP

#include "evo_log.hpp"
#include "usb_link.hpp"

namespace evo {

class UsbHost {
public:
    virtual ~UsbHost() {}
    virtual bool projectionBusy() = 0;
    virtual bool stopRequested() = 0;
    virtual void cableState(bool present, bool busy) = 0;
    virtual bool startCableSession(int socket) = 0;  // owns socket even when the start fails
};

inline void pollUsbLink(UsbLink& link, bool& carried, UsbHost& host, const std::atomic<bool>& quit) {
    if (carried && !link.active()) {
        const UsbBridge::Stats stats = link.stats();
        ELOG("Usb", "cable session over: %llu bytes from the phone, %llu to it, ended with %d",
             static_cast<unsigned long long>(stats.fromPhone), static_cast<unsigned long long>(stats.toPhone),
             stats.error);
        link.close();
        carried = false;
    }
    bool present = carried;
    if (!carried && !quit && !host.projectionBusy() && !host.stopRequested() && link.phoneWaiting(&present)) {
        host.cableState(true, true);  // wireless must not take the phone during the USB handshake
        const int socket = link.connect(quit);
        if (socket >= 0) {
            carried = true;
            if (!host.startCableSession(socket)) {
                link.close(kUsbStartFailureRetryMs);
                carried = false;
            }
        }
    }
    host.cableState(present || carried, carried);
}

}  // namespace evo
#endif  // AA_ENTRYNAV_PLATFORM_USB_LOOP_HPP
