// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The steering-wheel roller: its RotarySteps byte, and roll then push for one track skip.
#include <assert.h>
#include <stdio.h>

#include "wheel_roller.hpp"

int main() {
    // RotarySteps byte, as HmiMain3D reads it: below 0x7F down iDrive's lists (next), above it up (previous).
    assert(evo::rollerSteps(0x7F) == 0);
    assert(evo::rollerSteps(0xFF) == 0);
    assert(evo::rollerSteps(0x7E) == 1);
    assert(evo::rollerSteps(0x7D) == 2);
    assert(evo::rollerSteps(0x00) == 0x7F);
    assert(evo::rollerSteps(0x80) == -1);
    assert(evo::rollerSteps(0x81) == -2);
    assert(evo::rollerSteps(0xFE) == -0x7F);

    {  // Roll down then push: the next track, once. A second push without a roll does nothing.
        evo::WheelSkip w;
        w.rolled(evo::rollerSteps(0x7E), 1000);
        w.rolled(evo::rollerSteps(0x7D), 1120);
        assert(w.pending(1500) == 1);
        assert(w.pushed(2000) == 1);
        assert(w.pending(2000) == 0);
        assert(w.pushed(2500) == 0);
    }
    {  // Roll up then push: the previous track. The last roll's direction counts, however far it went.
        evo::WheelSkip w;
        w.rolled(3, 1000);
        w.rolled(-1, 1200);
        assert(w.pushed(1400) == -1);
    }
    {  // A roll alone skips nothing; a push after the list has closed does nothing either
        evo::WheelSkip w;
        w.rolled(1, 1000);
        assert(w.pending(1000 + evo::WheelSkip::kPushWithinMs + 1) == 0);
        assert(w.pushed(1000 + evo::WheelSkip::kPushWithinMs + 1) == 0);
        w.rolled(1, 10000);
        assert(w.pushed(10000 + evo::WheelSkip::kPushWithinMs) == 1);  // the limit itself still counts
    }
    {  // A push alone does nothing; no movement does not set a direction
        evo::WheelSkip w;
        assert(w.pushed(1000) == 0);
        w.rolled(0, 1100);
        assert(w.pushed(1200) == 0);
    }
    {  // After the reads: their answers (the last values) are not the driver; a roll and push after the settling are
        evo::WheelSkip w;
        w.rolled(1, 4000);
        w.reset(5000);
        assert(w.pushed(5010) == 0);  // the roll before the reset is gone too
        w.rolled(evo::rollerSteps(0x80), 5020);
        assert(w.pushed(5000 + evo::WheelSkip::kSettleMs - 1) == 0);
        w.rolled(1, 5000 + evo::WheelSkip::kSettleMs + 100);
        assert(w.pushed(5000 + evo::WheelSkip::kSettleMs + 300) == 1);
    }
    {  // The monotonic clock wraps between roll and push, and during the settling time
        evo::WheelSkip w;
        w.rolled(-1, 0xFFFFFF00u);
        assert(w.pushed(0x100) == -1);
        evo::WheelSkip s;
        s.reset(0xFFFFFF00u);
        s.rolled(1, 0x100);
        assert(s.pushed(0x200) == 0);
        s.rolled(1, 0x100 + evo::WheelSkip::kSettleMs);
        assert(s.pushed(0x200 + evo::WheelSkip::kSettleMs) == 1);
    }
    {  // With the cluster's skip menu (repeat): the roll sets the direction, then each push skips again, each within
       // kPushWithinMs of the one before; a new roll changes it, and a pause closes it
        evo::WheelSkip w;
        w.rolled(1, 1000);
        assert(w.pushed(1500, true) == 1);
        assert(w.pushed(1500 + evo::WheelSkip::kPushWithinMs, true) == 1);
        assert(w.pushed(1600 + evo::WheelSkip::kPushWithinMs, true) == 1);
        w.rolled(-1, 9000);
        assert(w.pushed(9100, true) == -1 && w.pushed(9200, true) == -1);
        assert(w.pushed(9201 + evo::WheelSkip::kPushWithinMs, true) == 0);
        assert(w.pushed(9300 + evo::WheelSkip::kPushWithinMs, true) == 0);  // closed: a roll first
        w.rolled(1, 20000);
        assert(w.pushed(20100) == 1 && w.pushed(20200) == 0);  // without repeat, one push per roll as before
    }
    printf("wheel roller: OK\n");
    return 0;
}
