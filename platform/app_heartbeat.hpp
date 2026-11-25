// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The iDrive app's heartbeat, as the service sees it. The app sends "alive <n>" from its status poll, once a second,
// while the service runs. A poll that stops means a thread of HmiMain3D's that stopped: in 0.1.9-test.1 and test.2 the
// app went quiet seconds before iDrive died, and nothing in the logs said where. Once the app has been quiet for
// kAppSilentMs, the service logs it with what HmiMain3D's threads wait on and the step the app was at
// (contract::kAppStepFile, hmiapp/app_step.hpp).
#ifndef AA_ENTRYNAV_PLATFORM_APP_HEARTBEAT_HPP
#define AA_ENTRYNAV_PLATFORM_APP_HEARTBEAT_HPP

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <string>

namespace evo {

const uint32_t kAppSilentMs = 5000;  // five missed polls

// alive() from the control thread, poll() from the main loop. Times are monotonic milliseconds, compared with
// wrap-around. Nothing is reported before the first "alive": an app older than the service, or none at all.
class AppHeartbeat {
public:
    enum Event { kNone, kSilent, kBack };

    AppHeartbeat() : lastMs_(0), seen_(false), silent_(false), silentSinceMs_(0) {}

    void alive(uint32_t nowMs) {
        lastMs_ = nowMs;
        seen_ = true;
    }

    // kSilent once when the app has been quiet for kAppSilentMs, kBack once when it sends again; *quietMs is how long
    // it has been quiet.
    Event poll(uint32_t nowMs, uint32_t* quietMs) {
        if (!seen_) {
            return kNone;
        }
        const uint32_t quiet = nowMs - lastMs_;
        if (quietMs) {
            *quietMs = quiet;
        }
        if (!silent_ && quiet >= kAppSilentMs && quiet < 0x80000000u) {
            silent_ = true;
            silentSinceMs_ = lastMs_;
            return kSilent;
        }
        if (silent_ && quiet < kAppSilentMs) {
            silent_ = false;
            if (quietMs) {
                *quietMs = lastMs_ - silentSinceMs_;
            }
            return kBack;
        }
        return kNone;
    }

private:
    std::atomic<uint32_t> lastMs_;
    std::atomic<bool> seen_;
    bool silent_;
    uint32_t silentSinceMs_;
};

// The first line of a small file, without its newline ("" if it can't be read).
inline std::string readFirstLine(const std::string& path) {
    char buf[128] = "";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        return std::string();
    }
    if (!fgets(buf, sizeof(buf), f)) {
        buf[0] = 0;
    }
    fclose(f);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
        buf[--n] = 0;
    }
    return buf;
}

// The app's step file (hmiapp/app_step.hpp): fixed-size lines of lineSize bytes, a line not written yet all zero. One
// line per written one, trailing spaces cut.
inline std::string readAppSteps(const char* path, int lineSize) {
    std::string out;
    FILE* f = fopen(path, "r");
    if (!f || lineSize <= 0 || lineSize > 256) {
        if (f) {
            fclose(f);
        }
        return out;
    }
    char line[256];
    while (fread(line, 1, static_cast<size_t>(lineSize), f) == static_cast<size_t>(lineSize)) {
        size_t n = strnlen(line, static_cast<size_t>(lineSize));
        while (n && (line[n - 1] == '\n' || line[n - 1] == ' ')) {
            --n;
        }
        if (n) {
            out.append(line, n);
            out += '\n';
        }
    }
    fclose(f);
    return out;
}

// The PID of the first process under procRoot ("/proc") whose comm is name, or 0.
inline int findProcess(const char* procRoot, const char* name) {
    DIR* d = opendir(procRoot);
    if (!d) {
        return 0;
    }
    int pid = 0;
    while (struct dirent* e = readdir(d)) {
        char* end = nullptr;
        const long v = strtol(e->d_name, &end, 10);
        if (v <= 0 || *end) {
            continue;
        }
        if (readFirstLine(std::string(procRoot) + "/" + e->d_name + "/comm") == name) {
            pid = static_cast<int>(v);
            break;
        }
    }
    closedir(d);
    return pid;
}

// One line per thread of pid: "<tid> <comm> <state> <wchan>", the kernel function a sleeping thread waits in (0 or
// empty when it runs). At most maxThreads lines.
inline std::string describeThreads(const char* procRoot, int pid, unsigned maxThreads = 64) {
    std::string out;
    char pidText[16];
    snprintf(pidText, sizeof(pidText), "%d", pid);
    const std::string dir = std::string(procRoot) + "/" + pidText + "/task";
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return out;
    }
    unsigned n = 0;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (n++ == maxThreads) {
            out += "...\n";
            break;
        }
        const std::string task = dir + "/" + e->d_name;
        // stat: "tid (comm) S ...": the state follows the last ')', as comm may hold spaces or parentheses.
        const std::string stat = readFirstLine(task + "/stat");
        const size_t close = stat.rfind(')');
        const char state = close != std::string::npos && close + 2 < stat.size() ? stat[close + 2] : '?';
        std::string wchan = readFirstLine(task + "/wchan");
        out += std::string(e->d_name) + " " + readFirstLine(task + "/comm") + " " + state + " " +
               (wchan.empty() ? "-" : wchan) + "\n";
    }
    closedir(d);
    return out;
}

}  // namespace evo

#endif
