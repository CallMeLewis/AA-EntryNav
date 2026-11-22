// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Which volume levels from the car open the volume pill, as iDrive opens its own bar: pure, no clock, no lock.
//
// Read upstream in the reference car's HmiMain3D (iDrive 202111011321) and libhmil6services.so (EntryNav2-Android-Auto
// #83, add966929e):
//  - iDrive files each level record by its minimum: 0 is the volume the knob turns, above 0 another source's. A MINI
//    F57 received "25 between 25 and 100" ten times in four seconds as the phone connected, while the knob's volume was
//    3 between 0 and 100, and the pill opened by itself with an empty bar. During a call the knob turns the call's
//    volume, so a level above 0 is kept then.
//  - The same level comes again with nothing changed. iDrive's bar opens on a change or a volume key, and so does the
//    pill.
// Which record of the AudioMaster's array is read is evo_knob's business (isEntertainmentVolume).
// tests/test_volume_filter.cpp.
#ifndef AA_ENTRYNAV_PLATFORM_VOLUME_FILTER_HPP
#define AA_ENTRYNAV_PLATFORM_VOLUME_FILTER_HPP

#include <stdint.h>

namespace evo {

class VolumeFilter {
public:
    static const uint32_t kKeyWindowMs = 1000;  // a level this soon after a volume key shows, changed or not

    VolumeFilter() : keyAt_(0) {}

    // A volume step from the knob or the steering wheel: the level that follows shows.
    void volumeKey(uint32_t now) { keyAt_ = now ? now : 1; }  // 0 means "no key yet"

    // A level reported by the car. True when the pill shows it. inCall: iDrive has a call (the call's own volume).
    bool level(unsigned value, unsigned min, unsigned max, bool inCall, uint32_t now) {
        const bool knob = min == 0;
        if (!knob && !inCall) {
            return false;  // another source's level
        }
        // Each kind remembered on its own: the knob's level as a call ends is not a change.
        Last& last = knob ? knob_ : call_;
        const bool changed = !last.known || value != last.value || min != last.min || max != last.max;
        last.known = true;
        last.value = value;
        last.min = min;
        last.max = max;
        return changed || (keyAt_ && now - keyAt_ <= kKeyWindowMs);  // wraps with the clock
    }

private:
    struct Last {
        bool known;
        unsigned value, min, max;
        Last() : known(false), value(0), min(0), max(0) {}
    };
    Last knob_, call_;
    uint32_t keyAt_;
};

}  // namespace evo

#endif
