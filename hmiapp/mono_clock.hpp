// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Time since the head unit started, for the app's delays. The car's date can be set at any moment (by GPS, or by hand
// in iDrive's settings), and a delay measured with it would end at once or last until the clock caught up again; this
// clock only ever moves forward, one second per second.
//
// Only what spans a restart stays on the date: the crash and boot guards (crash_guard.hpp, app_guards.cpp), whose
// files outlive the head unit's run, and the log's timestamps.
#ifndef AA_ENTRYNAV_HMIAPP_MONO_CLOCK_HPP
#define AA_ENTRYNAV_HMIAPP_MONO_CLOCK_HPP

#include <time.h>

namespace evo {
namespace app {

inline long monoMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
// Never 0: 0 means "none" for the app's timers (a call not ended, no driving mode change), and this clock starts near
// 0 when the head unit does.
inline long monoSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec) + 1;
}

}  // namespace app
}  // namespace evo

#endif
