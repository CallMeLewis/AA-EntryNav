// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// MEDIA with iDrive on screen while Android Auto's sound plays: when Android Auto comes forward on its media app.
#include <assert.h>
#include <stdio.h>

#include "media_key_jump.hpp"

int main() {
    {  // Our route built, a session, nothing in the way: the app is asked, and its "show" sends MEDIA once
        evo::MediaKeyJump j;
        assert(j.pressed(true, true, false, 1000));
        assert(j.shown(1500));
        assert(!j.shown(1600));  // a second "show" is not the press's
    }
    {  // Not ours to take: no session, the radio or Bluetooth playing (no route), a call or reverse gear
        evo::MediaKeyJump j;
        assert(!j.pressed(false, true, false, 1000));
        assert(!j.pressed(true, false, false, 1000));
        assert(!j.pressed(true, true, true, 1000));
        assert(!j.shown(1100));  // the row's own "show" sends nothing
    }
    {  // A "show" too late belongs to something else (the Open row, much later)
        evo::MediaKeyJump j;
        assert(j.pressed(true, true, false, 1000));
        assert(j.shown(1000 + evo::MediaKeyJump::kShowWithinMs));  // the limit itself still counts
        assert(j.pressed(true, true, false, 1000));
        assert(!j.shown(1001 + evo::MediaKeyJump::kShowWithinMs));
    }
    {  // "hide" or the session's end in between
        evo::MediaKeyJump j;
        assert(j.pressed(true, true, false, 1000));
        j.cancel();
        assert(!j.shown(1200));
    }
    {  // The monotonic clock wraps between the press and the "show"
        evo::MediaKeyJump j;
        assert(j.pressed(true, true, false, 0xFFFFFF00u));
        assert(j.shown(0x100));
    }
    {  // Pressed again: the later press counts
        evo::MediaKeyJump j;
        assert(j.pressed(true, true, false, 1000));
        assert(j.pressed(true, true, false, 6000));
        assert(j.shown(7000));
    }
    printf("media key jump: OK\n");
    return 0;
}
