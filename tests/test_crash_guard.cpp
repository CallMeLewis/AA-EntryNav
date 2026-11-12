// Copyright (c) 2026 CallMeLewis
// The crash guard's count (hmiapp/crash_guard.hpp), on the PC, in a temporary folder: it counts each load, trips at the
// maximum without counting further, counts a process once however often it loads the library, and keeps the app off
// whenever it cannot record a load.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hmiapp/crash_guard.hpp"

using namespace evo::app;

static int g_failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

static int linesIn(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    int n = 0;
    char b[64];
    while (fgets(b, sizeof(b), f)) {
        ++n;
    }
    fclose(f);
    return n;
}

int main() {
    char dir[] = "/tmp/aa-entrynav-guard-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char guard[256], missing[256], asDir[256];
    snprintf(guard, sizeof(guard), "%s/app-guard", dir);
    snprintf(missing, sizeof(missing), "%s/no-such-folder/app-guard", dir);
    snprintf(asDir, sizeof(asDir), "%s/a-folder", dir);
    int lines = -1, err = -1;

    // No file yet: the first load is allowed and recorded. Each iDrive process has its own identity.
    CHECK(countLoadIn(guard, 3, 1000, "boot-a-101", &lines, &err) == kGuardAllows);
    CHECK(lines == 0 && err == 0 && linesIn(guard) == 1);
    // The same process loading the library again (load, unload, load) is not counted twice.
    CHECK(countLoadIn(guard, 3, 1000, "boot-a-101", &lines, &err) == kGuardAllows);
    CHECK(lines == 1 && linesIn(guard) == 1);
    // A restarted iDrive (another pid), then a restarted unit (another boot, same pid): each recorded.
    CHECK(countLoadIn(guard, 3, 1001, "boot-a-202", &lines, &err) == kGuardAllows);
    CHECK(lines == 1 && linesIn(guard) == 2);
    CHECK(countLoadIn(guard, 3, 1002, "boot-b-101", &lines, &err) == kGuardAllows);
    CHECK(lines == 2 && linesIn(guard) == 3);
    // The third process is the last one allowed, and stays allowed when it loads the library again.
    CHECK(countLoadIn(guard, 3, 1002, "boot-b-101", &lines, &err) == kGuardAllows);
    CHECK(lines == 3 && linesIn(guard) == 3);
    // The fourth process within the crash loop: refused, each time, and the file is not grown any further.
    CHECK(countLoadIn(guard, 3, 1003, "boot-c-101", &lines, &err) == kGuardTripped);
    CHECK(lines == 3 && linesIn(guard) == 3);
    CHECK(countLoadIn(guard, 3, 1004, "boot-c-101", &lines, &err) == kGuardTripped);
    CHECK(linesIn(guard) == 3);
    // An identity that is only the end of a recorded one is another process.
    CHECK(countLoadIn(guard, 3, 1005, "a-101", &lines, &err) == kGuardTripped);
    // The stable run deletes the file: counting starts again.
    unlink(guard);
    CHECK(countLoadIn(guard, 3, 2000, "boot-c-101", &lines, &err) == kGuardAllows);
    CHECK(lines == 0 && linesIn(guard) == 1);

    // Without an identity (boot id unreadable), every load counts, null or empty alike.
    unlink(guard);
    CHECK(countLoadIn(guard, 3, 2100, "", &lines, &err) == kGuardAllows);
    CHECK(countLoadIn(guard, 3, 2101, nullptr, &lines, &err) == kGuardAllows);
    CHECK(countLoadIn(guard, 3, 2102, "", &lines, &err) == kGuardAllows);
    CHECK(linesIn(guard) == 3);
    CHECK(countLoadIn(guard, 3, 2103, "", &lines, &err) == kGuardTripped);
    // Lines written by the previous version of the app (a time alone) count as loads of other processes.
    FILE* old = fopen(guard, "w");
    CHECK(old != nullptr);
    if (old) {
        fprintf(old, "1000\n1001\n");
        fclose(old);
    }
    CHECK(countLoadIn(guard, 3, 2200, "boot-d-101", &lines, &err) == kGuardAllows);
    CHECK(lines == 2 && linesIn(guard) == 3);
    CHECK(countLoadIn(guard, 3, 2201, "boot-d-202", &lines, &err) == kGuardTripped);
    unlink(guard);

    // Fails closed: a folder that does not exist, and a path that is a folder, both keep the app off.
    CHECK(countLoadIn(missing, 3, 3000, "boot-a-101", &lines, &err) == kGuardUnwritable);
    CHECK(err == ENOENT);
    CHECK(mkdir(asDir, 0755) == 0);
    CHECK(countLoadIn(asDir, 3, 3000, "boot-a-101", &lines, &err) == kGuardUnwritable);
    CHECK(err != 0);

    // A file that cannot be written (read-only, as root ignores modes only when not root).
    if (geteuid() != 0) {
        char ro[256];
        snprintf(ro, sizeof(ro), "%s/read-only", dir);
        FILE* f = fopen(ro, "w");
        CHECK(f != nullptr);
        if (f) {
            fclose(f);
        }
        CHECK(chmod(ro, 0444) == 0);
        CHECK(countLoadIn(ro, 3, 4000, "boot-a-101", &lines, &err) == kGuardUnwritable);
        CHECK(err == EACCES);
        unlink(ro);
    }
    // A long file is read no further than the maximum.
    FILE* big = fopen(guard, "w");
    CHECK(big != nullptr);
    if (big) {
        for (int i = 0; i < 10000; ++i) {
            fprintf(big, "%d\n", i);
        }
        fclose(big);
    }
    CHECK(countLoadIn(guard, 3, 5000, "boot-a-101", &lines, &err) == kGuardTripped);
    CHECK(lines == 3);

    unlink(guard);
    rmdir(asDir);
    rmdir(dir);
    if (g_failures) {
        fprintf(stderr, "test_crash_guard: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("PASS test_crash_guard\n");
    return 0;
}
