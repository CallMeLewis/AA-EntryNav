// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Production writes the service's output nowhere; the marker file turns the logs on and off without a restart.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hmiapp/log_profile.hpp"

int main() {
    char marker[] = "/tmp/aa-entrynav-profile-XXXXXX";
    FILE* created = fdopen(mkstemp(marker), "w");
    assert(created);
    fclose(created);
    assert(maintenanceProfile(marker));
    assert(!strcmp(serviceLogPath(marker, "service.log"), "service.log"));
    assert(!unlink(marker));
    assert(!maintenanceProfile(marker));
    assert(!strcmp(serviceLogPath(marker, "service.log"), "/dev/null"));

    // The service's log, appended to by another process, kept within its limit.
    char log[] = "/tmp/aa-entrynav-servicelog-XXXXXX";
    int writer = mkstemp(log);
    assert(writer >= 0);
    close(writer);
    writer = open(log, O_WRONLY | O_APPEND);  // as the service holds it
    assert(writer >= 0);
    char old[64];
    snprintf(old, sizeof(old), "%s.old", log);
    assert(write(writer, "0123456789", 10) == 10);
    assert(!boundServiceLog(log, old, 16));  // under the limit: left alone
    assert(access(old, F_OK) != 0);
    assert(write(writer, "abcdefghij", 10) == 10);
    assert(boundServiceLog(log, old, 16));  // past it: copied, then emptied
    struct stat st;
    assert(stat(log, &st) == 0 && st.st_size == 0);
    assert(stat(old, &st) == 0 && st.st_size == 20);
    assert(write(writer, "next", 4) == 4);  // the writer carries on at the start of the emptied file
    assert(stat(log, &st) == 0 && st.st_size == 4);
    assert(!boundServiceLog("/tmp/aa-entrynav-no-such-log", old, 16));
    close(writer);
    unlink(log);
    unlink(old);

    // Root's files in /tmp never written through a link another user left there (platform/tmp_file.hpp).
    char victim[] = "/tmp/aa-entrynav-victim-XXXXXX";
    int v = mkstemp(victim);
    assert(v >= 0 && write(v, "keep", 4) == 4);
    close(v);
    char link[64];
    snprintf(link, sizeof(link), "%s.link", victim);
    assert(!symlink(victim, link));
    assert(evo::appendTmpFile(link, 0644) < 0);         // appending through the link: refused
    assert(!evo::emptyTmpFile(link));                   // emptying through it: refused
    const int fresh = evo::replaceTmpFile(link, 0644);  // replacing: the link goes, a new file takes its name
    assert(fresh >= 0 && write(fresh, "new", 3) == 3);
    close(fresh);
    struct stat lst;
    assert(lstat(link, &lst) == 0 && S_ISREG(lst.st_mode) && lst.st_size == 3);
    assert(stat(victim, &st) == 0 && st.st_size == 4);  // the file it pointed at is untouched
    unlink(link);
    unlink(victim);
    puts(
        "PASS production profile without logs, maintenance profile with logs, service log kept within its limit, "
        "no write through a link in /tmp");
    return 0;
}
