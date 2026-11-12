// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The off switch, the crash guard, the boot-loop guard and the automatic start (app_guards.hpp).
#include "app_guards.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../platform/service_contract.hpp"
#include "app_log.hpp"
#include "crash_guard.hpp"
#include "mono_clock.hpp"
#include "service_link.hpp"

namespace evo {
namespace app {

namespace {

const long kAutostartDelayS = 30;
const int kBootGuardMax = 3;
const long kBootGuardWindowS = 15 * 60;
const int kCrashGuardMax = 3;  // starts without 2 min of uptime before the app turns itself off
const long kStableAfterS = 120;

const int kNotCounted = 2;  // g_loadVerdict before countLoad(); then a CrashGuardVerdict

bool g_autostartDone = false, g_stableMarked = false;
int g_loadVerdict = kNotCounted;

bool fileExists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

bool bootGuardAllows() {
    time_t now = time(nullptr);
    long t[8];
    int n = 0;
    FILE* f = fopen(contract::kBootGuard, "r");
    if (f) {
        long v;
        while (n < 8 && fscanf(f, "%ld", &v) == 1) {
            if (v <= now && now - v < kBootGuardWindowS) {
                t[n++] = v;
            }
        }
        fclose(f);
    }
    if (n >= kBootGuardMax) {
        alog("autostart SKIPPED by the bootloop guard (%d starts within %ld min): use Start in the menu", n,
             kBootGuardWindowS / 60);
        return false;
    }
    f = fopen(contract::kBootGuard, "w");
    if (f) {
        for (int i = 0; i < n; ++i) {
            fprintf(f, "%ld\n", t[i]);
        }
        fprintf(f, "%ld\n", static_cast<long>(now));
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    return true;
}

}  // namespace

bool appDisabled() {
    if (fileExists(contract::kAppDisable)) {
        alog("disabled by %s", contract::kAppDisable);
        return true;
    }
    return false;
}

// ---- crash guard (HMI crash loop protection) ------------------------------------------------------------------------
// countLoad() appends one line as soon as HmiMain3D loads the library (its constructor), so a crash between the load
// and create() counts too; 2 minutes later the status poll deletes the file. kCrashGuardMax lines = the HMI went down
// that many times shortly after loading us: create() then refuses (iDrive starts without the app). A load that cannot
// be recorded is refused as well (crash_guard.hpp).

namespace {
// This HMI process, in this boot: the kernel's boot id and our pid, so that a library HmiMain3D loads twice in one
// start counts once, while a restarted HmiMain3D (another pid) or a restarted unit (another boot id, often the same
// pid) counts again. Left empty when the boot id cannot be read: every load counts then.
void loadIdentity(char* out, size_t size) {
    out[0] = 0;
    char boot[64] = "";
    FILE* f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!f) {
        return;
    }
    if (!fgets(boot, sizeof(boot), f)) {
        boot[0] = 0;
    }
    fclose(f);
    boot[strcspn(boot, " \r\n")] = 0;
    if (boot[0]) {
        snprintf(out, size, "%s-%d", boot, static_cast<int>(getpid()));
    }
}
}  // namespace

void countLoad() {
    if (g_loadVerdict != kNotCounted) {
        return;  // once per load of the library
    }
    char identity[96];
    loadIdentity(identity, sizeof(identity));
    int lines = 0, err = 0;
    g_loadVerdict = countLoadIn(contract::kAppGuard, kCrashGuardMax, time(nullptr), identity, &lines, &err);
    if (g_loadVerdict == kGuardTripped) {
        alog(
            "crash guard: the HMI restarted %d times shortly after loading this app: app turned off. "
            "Remove %s after checking the logs, then restart the unit.",
            lines, contract::kAppGuard);
    } else if (g_loadVerdict == kGuardUnwritable) {
        alog("crash guard: cannot record this load in %s (errno %d): app kept off", contract::kAppGuard, err);
    }
}

bool crashGuardAllows() {
    countLoad();
    return g_loadVerdict == kGuardAllows;
}

void resetGuardsWhenStable(long initTime) {
    if (!g_stableMarked && initTime && monoSeconds() - initTime >= kStableAfterS) {
        g_stableMarked = true;
        unlink(contract::kAppGuard);  // the HMI has been up 2 min with us: not a crash loop
        unlink(contract::kBootGuard);
        alog("stable for %ld s: crash and boot guards reset", kStableAfterS);
    }
}

// ---- start automatically (with a bootloop guard) --------------------------------------------------------------------
void serviceAutostart(long initTime) {
    if (g_autostartDone || !initTime || monoSeconds() - initTime < kAutostartDelayS) {
        return;
    }
    g_autostartDone = true;
    if (!autostartOn()) {
        return;
    }
    ServiceState d = readServiceState();
    if (d.pid != 0) {
        alog("autostart: service already running (pid %d)", d.pid);
        return;
    }
    if (!bootGuardAllows()) {
        return;
    }
    alog("autostart: launching the service");
    launchService();
}

void forgetBootGuard() {
    unlink(contract::kBootGuard);
}

bool autostartOn() {
    return fileExists(contract::kAutostart);
}

void toggleAutostart() {
    bool on = autostartOn();
    if (on) {
        if (unlink(contract::kAutostart) != 0) {
            alog("autostart: cannot remove %s (errno %d)", contract::kAutostart, errno);
        }
    } else {
        int fd = open(contract::kAutostart, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            alog("autostart: cannot create %s (errno %d)", contract::kAutostart, errno);
        } else {
            ssize_t w = write(fd, "1\n", 2);
            (void)w;
            fsync(fd);
            close(fd);
        }
    }
    alog("menu: start automatically %s -> %s", on ? "on" : "off", autostartOn() ? "on" : "off");
}

}  // namespace app
}  // namespace evo
