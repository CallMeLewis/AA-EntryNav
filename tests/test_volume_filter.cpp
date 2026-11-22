// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Which volume levels open the pill (platform/volume_filter.hpp): changes and keys, never another source's level.
#include <stdio.h>
#include <stdlib.h>

#include "volume_filter.hpp"

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)

namespace {

void atClock(uint32_t t) {
    evo::VolumeFilter v;
    // The MINI F57 as the phone connects: another source's level, again and again, while the knob's volume is 3.
    for (unsigned i = 0; i < 10; ++i) {
        CHECK(!v.level(25, 25, 100, false, t + 300 * i));
    }
    // The first level of the knob's volume shows; the same one again, nothing having changed, does not.
    CHECK(v.level(3, 0, 100, false, t + 4000));
    CHECK(!v.level(3, 0, 100, false, t + 4300));
    // A change shows, and so does a new range.
    CHECK(v.level(5, 0, 100, false, t + 5000));
    CHECK(v.level(5, 0, 40, false, t + 5100));
    CHECK(!v.level(5, 0, 40, false, t + 5200));
    // A volume key: the next level shows even unchanged (the knob at its end stop), not one past the window.
    v.volumeKey(t + 6000);
    CHECK(v.level(5, 0, 40, false, t + 6100));
    CHECK(v.level(5, 0, 40, false, t + 6000 + evo::VolumeFilter::kKeyWindowMs));
    CHECK(!v.level(5, 0, 40, false, t + 6000 + evo::VolumeFilter::kKeyWindowMs + 1));
    // Another source's level right after a key still does not show.
    v.volumeKey(t + 7000);
    CHECK(!v.level(25, 25, 100, false, t + 7100));
    // During a call the knob turns the call's volume, whose minimum can be above 0: it shows on a change or a key.
    CHECK(v.level(4, 1, 10, true, t + 9000));
    CHECK(!v.level(4, 1, 10, true, t + 9300));
    v.volumeKey(t + 9500);
    CHECK(v.level(4, 1, 10, true, t + 9600));
    // The call ends and the knob's level comes again, unchanged since before the call: no pill.
    CHECK(!v.level(5, 0, 40, false, t + 12000));
    CHECK(!v.level(4, 1, 10, false, t + 12100));  // the call's level after it ended is another source's
}

}  // namespace

int main() {
    atClock(1000);
    atClock(0xFFFFF000u);  // across the 32-bit wrap of the millisecond clock
    atClock(0);            // a key at time 0 still counts
    puts("PASS volume filter: another source's level ignored, repeats held back, a level after a key shown, calls");
    return 0;
}
