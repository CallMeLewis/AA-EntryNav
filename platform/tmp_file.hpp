// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Files the service and the iDrive app write in /tmp as root. Any user on the head unit can create a name there first,
// a symbolic link to a system file for instance, so these never write through a link: what is replaced is unlinked and
// created afresh (O_EXCL), and what is appended to is opened with O_NOFOLLOW. tests/test_log_profile.cpp.
#ifndef AA_ENTRYNAV_PLATFORM_TMP_FILE_HPP
#define AA_ENTRYNAV_PLATFORM_TMP_FILE_HPP

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace evo {

// A new, empty file at path, whatever was there: a descriptor to write, or -1 (with errno) when someone else made the
// name again in between. The caller writes and closes it, then usually renames it into place.
inline int replaceTmpFile(const char* path, mode_t mode) {
    unlink(path);
    return open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
}

// path opened to append, created if missing, but not when it is a symbolic link: -1 then.
inline int appendTmpFile(const char* path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, mode);
}

// path emptied in place, not when it is a symbolic link. True when emptied.
inline bool emptyTmpFile(const char* path) {
    const int fd = open(path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool emptied = ftruncate(fd, 0) == 0;
    close(fd);
    return emptied;
}

}  // namespace evo

#endif
