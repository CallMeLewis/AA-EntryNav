// Copyright (c) 2026 CallMeLewis
// The app's step file (app_step.hpp).
#include "app_step.hpp"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#include "../platform/service_contract.hpp"

namespace evo {
namespace app {

namespace {

const int kLine = contract::kAppStepLine;
const char* const kSlotNames[] = {"status poll", "hmi job, HmiMain3D", "hmi job, cluster"};
std::atomic<int> g_fd(-2);  // -2 not opened yet, -1 could not be opened

int stepFile() {
    int fd = g_fd;
    if (fd != -2) {
        return fd;
    }
    fd = open(contract::kAppStepFile, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int expected = -2;
    if (!g_fd.compare_exchange_strong(expected, fd)) {
        if (fd >= 0) {
            close(fd);  // another thread opened it first
        }
        return expected;
    }
    return fd;
}

}  // namespace

void appStep(AppStepSlot slot, const char* step) {
    const int fd = stepFile();
    if (fd < 0) {
        return;
    }
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[kLine];
    int n = snprintf(line, sizeof(line), "%s: %s (at %ld s)", kSlotNames[slot], step, static_cast<long>(ts.tv_sec));
    if (n < 0) {
        return;
    }
    if (n > kLine - 1) {
        n = kLine - 1;
    }
    memset(line + n, ' ', kLine - 1 - n);
    line[kLine - 1] = '\n';
    if (pwrite(fd, line, kLine, static_cast<off_t>(slot) * kLine) != kLine) {
        return;  // nothing to do about it: the step is only a clue
    }
}

void closeAppStep() {
    const int fd = g_fd.exchange(-2);
    if (fd >= 0) {
        close(fd);
    }
}

}  // namespace app
}  // namespace evo
