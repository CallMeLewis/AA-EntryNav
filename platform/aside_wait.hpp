// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// How long a phone waits aside for the iDrive app's "show": pure timing, no clock, no lock.
//
// Once the iDrive app has said "await", a phone that connects is kept aside until the app brings iDrive to its blank
// state and says "show". The app sends that within a few seconds, even when iDrive refuses its pages, and waits only
// through a call or the reverse gear. A "show" that never comes is an app that died, an HMI that restarted without
// it, or a message dropped on the way: after kAsideWaitMs outside a call and reverse, the service shows Android Auto
// by itself, as it did before "await". Times are monotonic milliseconds (evo::monoMs), compared with wrap-around.
// tests/test_aside_wait.cpp.
#ifndef AA_ENTRYNAV_PLATFORM_ASIDE_WAIT_HPP
#define AA_ENTRYNAV_PLATFORM_ASIDE_WAIT_HPP

#include <stdint.h>

namespace evo {

const uint32_t kAsideWaitMs = 15000;  // no "show" for this long, outside a call and reverse: shown without it

class AsideWait {
public:
    AsideWait() : sinceMs_(0) {}

    // A session starts aside.
    void begin(uint32_t nowMs) { sinceMs_ = nowMs; }
    // Each turn of the main loop while the session is still aside. paused: a call or the reverse gear, through which
    // the app waits on purpose; the time starts again when it ends. True once the wait is over.
    bool expired(uint32_t nowMs, bool paused) {
        if (paused) {
            sinceMs_ = nowMs;
            return false;
        }
        return nowMs - sinceMs_ >= kAsideWaitMs;
    }

private:
    uint32_t sinceMs_;
};

}  // namespace evo

#endif
