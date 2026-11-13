// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A phone waiting aside for the iDrive app's "show" (platform/aside_wait.hpp): the limit, calls, reverse, clock wrap.
#include <stdio.h>

#include "aside_wait.hpp"

#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            return false;                                        \
        }                                                        \
    } while (0)

namespace {

bool waits(uint32_t t) {
    {  // no "show": shown without it once the limit is reached
        evo::AsideWait w;
        w.begin(t);
        CHECK(!w.expired(t, false));
        CHECK(!w.expired(t + evo::kAsideWaitMs - 1, false));
        CHECK(w.expired(t + evo::kAsideWaitMs, false));
    }
    {  // a call or the reverse gear: the app waits on purpose, the time starts again when it ends
        evo::AsideWait w;
        w.begin(t);
        CHECK(!w.expired(t + 10000, false));
        CHECK(!w.expired(t + 60000, true));  // a minute-long call
        CHECK(!w.expired(t + 60000 + evo::kAsideWaitMs - 1, false));
        CHECK(w.expired(t + 60000 + evo::kAsideWaitMs, false));
    }
    {  // a new session starts its own wait
        evo::AsideWait w;
        w.begin(t);
        w.begin(t + 20000);
        CHECK(!w.expired(t + 20000 + evo::kAsideWaitMs - 1, false));
    }
    return true;
}

}  // namespace

int main() {
    if (!waits(1000) || !waits(0xFFFFFFFFu - 5000u)) {
        return 1;
    }
    puts("PASS aside wait: shown without \"show\" after the limit, calls and reverse pause it, clock wrap");
    return 0;
}
