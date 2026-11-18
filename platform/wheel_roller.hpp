// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The steering-wheel roller skips Android Auto's track while its sound plays over Wi-Fi: roll up or down, then push.
//
// The roller reports on ControlElementsMFL 0x15.02. RotarySteps (0xC30), Status, one byte: HmiMain3D's
// CHmiControlElementsMFLController::onRotaryStepsStatus (0x2567178) ignores 0x7F and 0xFF, sends a value below 0x7F as
// (0x7F - value) steps of event 0x36 and a value above as (value - 0x7F) steps of event 0x37. The cluster's lists move
// one row down for 0x36 and one row up for 0x37 (CHmiICListInputHandler::handleInput, 0x35fdcb4). RotaryPress (0xC31),
// Status, one byte: 1 pressed, 0 released (onRotaryPressStatus, 0x2567348, event 0x38), which plays the row the roll
// reached. So, as with iDrive's own players: roll down then push is the next track, roll up then push the previous.
//
// iDrive cannot do it itself: with sound over Wi-Fi it takes CarPlay's audio route, but its entertainment side finds no
// CarPlay player without an iPhone (CEntModeHandlerBase::calculateModeHandlerAvailability needs CARPLAY's
// AuxIn_DeviceInfo2 DeviceStatus 2), so the cluster says "no active entertainment source" and plays nothing. See
// docs/aa-media-source-re.md.
//
// With no list to show how far it went, a push skips one track, in the direction of the last roll. A push more than
// kPushWithinMs after the last roll (iDrive's cluster list has closed by then) or without a roll does nothing. The
// reads that open vmostd's filter are answered with the roller's last values, which are not a roll: what comes within
// kSettleMs of reset() is ignored.
//
// Pure: no clock, no vMOST. The caller gives the time. tests/test_wheel_roller.cpp.
#ifndef AA_ENTRYNAV_PLATFORM_WHEEL_ROLLER_HPP
#define AA_ENTRYNAV_PLATFORM_WHEEL_ROLLER_HPP

#include <stdint.h>

namespace evo {

const uint16_t kMflRotaryStepsFkt = 0xC30, kMflRotaryPressFkt = 0xC31;

// The roller's RotarySteps byte as signed steps: positive is down iDrive's lists (next), negative is up (previous), 0
// no movement.
inline int rollerSteps(uint8_t v) {
    if (v == 0x7F || v == 0xFF) {
        return 0;
    }
    return v < 0x7F ? 0x7F - v : -(v - 0x7F);
}

class WheelSkip {
public:
    enum : uint32_t {
        kPushWithinMs = 4000,  // iDrive shows its cluster list this long after the last roll (showMenu, 0xFA0)
        kSettleMs = 1500       // after reset(): the answers to our reads, not the driver
    };

    WheelSkip() : rolledMs_(0), settleFromMs_(0), direction_(0), settling_(false) {}

    // The reads were sent: what comes in the next kSettleMs is the roller's last state.
    void reset(uint32_t nowMs) {
        settleFromMs_ = nowMs;
        settling_ = true;
        direction_ = 0;
    }

    // A RotarySteps status with these steps (rollerSteps): remembered until a push.
    void rolled(int steps, uint32_t nowMs) {
        if (settled(nowMs) && steps != 0) {
            direction_ = steps > 0 ? 1 : -1;
            rolledMs_ = nowMs;
        }
    }

    // The roller pushed. 1: skip to the next track, -1: the previous one, 0: nothing.
    int pushed(uint32_t nowMs) {
        if (!settled(nowMs)) {
            return 0;
        }
        const int d = nowMs - rolledMs_ <= kPushWithinMs ? direction_ : 0;
        direction_ = 0;
        return d;
    }

    // The direction a push would skip now (1, -1, 0), for the log.
    int pending(uint32_t nowMs) const { return nowMs - rolledMs_ <= kPushWithinMs ? direction_ : 0; }

private:
    bool settled(uint32_t nowMs) {
        if (settling_ && nowMs - settleFromMs_ < kSettleMs) {
            return false;
        }
        settling_ = false;
        return true;
    }

    uint32_t rolledMs_;
    uint32_t settleFromMs_;
    int direction_;
    bool settling_;
};

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_WHEEL_ROLLER_HPP
