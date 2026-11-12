// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// hmiapp/popup_peek.hpp: when Android Auto steps aside for an iDrive popup.
#include <assert.h>
#include <stdio.h>

#include "../hmiapp/popup_peek.hpp"

int main() {
    {  // a popup: one start, no repeat, one end
        PopupPeek p;
        assert(p.step(false, 100, 0) == PopupPeek::kNone && !p.active());
        assert(p.step(true, 101, 0) == PopupPeek::kStart && p.active());
        assert(p.step(true, 102, 0) == PopupPeek::kNone);
        assert(p.step(false, 103, 0) == PopupPeek::kEnd && !p.active());
        assert(p.step(false, 104, 0) == PopupPeek::kNone);
    }
    {  // the driving mode display gets its own shorter peek, once for the whole of its span
        PopupPeek p;
        assert(p.step(false, 199, 198) == PopupPeek::kNone);  // the mode changed, no display (iDrive's setting off)
        assert(p.step(true, 200, 198) == PopupPeek::kDriveMode && !p.active());
        assert(p.step(true, 201, 198) == PopupPeek::kNone && !p.active());
        assert(p.step(true, 207, 198) == PopupPeek::kNone && !p.active());  // still open past the 6 s: not taken again
        assert(p.step(false, 208, 198) == PopupPeek::kNone);
        assert(p.step(true, 220, 198) == PopupPeek::kStart);  // a later popup is a real one
    }
    {  // a long popup does not take the picture again once its time is over; a new span does
        PopupPeek p;
        assert(p.step(true, 300, 0) == PopupPeek::kStart);
        for (long t = 301; t < 370; ++t) {
            assert(p.step(true, t, 0) == PopupPeek::kNone);
        }
        assert(p.step(false, 370, 0) == PopupPeek::kEnd);
        assert(p.step(true, 400, 0) == PopupPeek::kStart);
    }
    puts("PASS iDrive popups: one step aside per popup, the driving mode display's own, no repeat");
    return 0;
}
