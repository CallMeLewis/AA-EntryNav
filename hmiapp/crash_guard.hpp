// Copyright (c) 2026 CallMeLewis
// The crash guard's count, on its own so the PC tests can run it: one line per iDrive process that loaded the app, in
// a file of the data partition, and a verdict. It fails closed: a load it cannot record is a load it refuses, since an
// uncounted load is one the guard could never stop. tests/test_crash_guard.cpp.
#ifndef AA_ENTRYNAV_HMIAPP_CRASH_GUARD_HPP
#define AA_ENTRYNAV_HMIAPP_CRASH_GUARD_HPP

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace evo {
namespace app {

enum CrashGuardVerdict {
    kGuardAllows = 1,      // counted: this load goes on
    kGuardTripped = 0,     // max loads already counted without a stable run: the app stays off
    kGuardUnwritable = -1  // the load could not be recorded (errno in *err): the app stays off
};

// Keeps the errno of the first step that failed.
inline void noteFailure(bool ok, int* err) {
    if (!ok && !*err) {
        *err = errno ? errno : EIO;
    }
}

// Counts one load in the file at path: refuses without counting once it holds max lines, otherwise appends
// "<time> <identity>" and syncs it to the flash before answering. identity names the process for this boot, without
// spaces: a process already in the file is allowed without a second line, so a library loaded twice by one iDrive
// start counts once. Empty or null when unknown: every load counts then. *lines receives the lines found before this
// load (no more than max are read), *err the errno of a failed write (0 otherwise).
inline CrashGuardVerdict countLoadIn(const char* path, int max, time_t now, const char* identity, int* lines,
                                     int* err) {
    *lines = 0;
    *err = 0;
    const bool known = identity && *identity;
    bool counted = false;
    FILE* f = fopen(path, "r");
    if (f) {
        char b[160];
        while (*lines < max && fgets(b, sizeof(b), f)) {  // no further: iDrive is starting meanwhile
            ++*lines;
            b[strcspn(b, "\r\n")] = 0;
            const char* space = strchr(b, ' ');
            if (known && space && !strcmp(space + 1, identity)) {
                counted = true;
            }
        }
        fclose(f);
    }
    if (counted) {
        return kGuardAllows;
    }
    if (*lines >= max) {
        return kGuardTripped;
    }
    f = fopen(path, "a");
    if (!f) {
        *err = errno ? errno : EIO;
        return kGuardUnwritable;
    }
    // Each step runs even after a failure, and the first failure's errno is the one reported.
    bool written = fprintf(f, "%ld %s\n", static_cast<long>(now), known ? identity : "-") > 0;
    noteFailure(written, err);
    noteFailure(fflush(f) == 0, err);
    noteFailure(fsync(fileno(f)) == 0, err);
    noteFailure(fclose(f) == 0, err);
    return *err ? kGuardUnwritable : kGuardAllows;
}

}  // namespace app
}  // namespace evo

#endif
