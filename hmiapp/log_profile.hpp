// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Production keeps no log, on flash or in memory. The maintenance profile is one file on the data partition,
// created at installation or by hand: the app and the service then log as usual. tests/test_log_profile.cpp.
#ifndef AA_ENTRYNAV_HMIAPP_LOG_PROFILE_HPP
#define AA_ENTRYNAV_HMIAPP_LOG_PROFILE_HPP
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../platform/tmp_file.hpp"

inline bool maintenanceProfile(const char* marker) {
    return access(marker, F_OK) == 0;
}

inline const char* serviceLogPath(const char* marker, const char* log) {
    return maintenanceProfile(marker) ? log : "/dev/null";
}

// The service's log is in /tmp, which is RAM on the head unit, and the service keeps appending to it (O_APPEND) for as
// long as it runs. Past limit bytes it is copied to oldPath and emptied in place: the service's next writes start it
// again, so the two files never hold more than about twice the limit. Lines written between the copy and the emptying
// are lost. True when it was emptied.
inline bool boundServiceLog(const char* path, const char* oldPath, off_t limit) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= limit) {
        return false;
    }
    int in = open(path, O_RDONLY | O_CLOEXEC);
    if (in >= 0) {
        int out = evo::replaceTmpFile(oldPath, 0644);
        if (out >= 0) {
            char buffer[16384];
            ssize_t n;
            while ((n = read(in, buffer, sizeof(buffer))) > 0) {
                if (write(out, buffer, static_cast<size_t>(n)) != n) {
                    break;
                }
            }
            close(out);
        }
        close(in);
    }
    // Emptied even when the copy failed: memory comes before an old copy of the log.
    return evo::emptyTmpFile(path);
}

#endif
