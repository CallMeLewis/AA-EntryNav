// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's sound route for music over Wi-Fi (see audio_route.hpp).
#include "audio_route.hpp"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "evo_log.hpp"
#include "evo_process.hpp"
#include "vmost_client.hpp"

namespace evo {

namespace {
const char kTag[] = "AudioRoute";

struct Exchange {
    VmostClient vmost;
    uint16_t fkt, handle;
    int answer;  // the op of the car's final answer, -1 before it
    unsigned processing;
    Exchange() : fkt(0), handle(0), answer(-1), processing(0) {}
};

void onMessage(void* msg, void* user) {
    Exchange* x = static_cast<Exchange*>(user);
    VmostMsgView v;
    if (!x || !parseVmostMsg(msg, v) || v.fblock != kConnectionMasterFblock || v.inst != kConnectionMasterInst ||
        v.fkt != x->fkt || v.len < 2 || ((v.data[0] << 8) | v.data[1]) != x->handle) {
        return;
    }
    if (v.op == 0xA) {
        ++x->processing;  // ProcessingAck: still working on it
    } else if (v.op == 0xD || v.op == 0x9) {
        x->answer = static_cast<int>(v.op);
    }
}

std::string selfPath() {
    char path[256];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) {
        return std::string();
    }
    path[n] = 0;
    return path;
}

struct Request {
    bool build;
    uint16_t route;
    RouteDone done;
    void* user;
    bool queued;      // waits its turn; the service's last release, as it stops, does not
    unsigned ticket;  // its place in line
};

RouteRequester g_requester = nullptr;

// One request at a time, in the order made: a ticket each, served in turn.
pthread_mutex_t g_lineMu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_lineCv = PTHREAD_COND_INITIALIZER;
unsigned g_nextTicket = 0, g_serving = 0;

void* runRequest(void* arg) {
    Request* r = static_cast<Request*>(arg);
    if (r->queued) {
        pthread_mutex_lock(&g_lineMu);
        while (g_serving != r->ticket) {
            pthread_cond_wait(&g_lineCv, &g_lineMu);
        }
        pthread_mutex_unlock(&g_lineMu);
    }
    char route[8];
    snprintf(route, sizeof(route), "%x", r->route);
    const std::string self = selfPath();
    std::vector<std::string> args;
    args.push_back("--audio-route");
    args.push_back(r->build ? "build" : "remove");
    args.push_back(route);
    std::string output;
    CommandReport report;
    const bool ok = !self.empty() && runCommand(self, args, output, 20000, &report);
    // The helper's own lines, then the outcome.
    size_t from = 0;
    while (from < output.size()) {
        size_t end = output.find('\n', from);
        if (end == std::string::npos) {
            end = output.size();
        }
        if (end > from) {
            ELOG(kTag, "%s", output.substr(from, end - from).c_str());
        }
        from = end + 1;
    }
    if (!ok && (self.empty() || report.timedOut || report.spawnError || report.signal)) {
        ELOG(kTag, "route 0x%x %s: the helper did not finish (exit %d%s)", r->route, r->build ? "build" : "release",
             report.exitCode, report.timedOut ? ", timed out" : "");
    }
    if (r->queued) {
        pthread_mutex_lock(&g_lineMu);
        ++g_serving;
        pthread_cond_broadcast(&g_lineCv);
        pthread_mutex_unlock(&g_lineMu);
    }
    if (r->done) {
        r->done(ok && report.exitCode == 0, r->user);
    }
    delete r;
    return nullptr;
}
}  // namespace

void buildRoutePayload(uint8_t out[4], uint16_t handle, uint16_t route) {
    out[0] = static_cast<uint8_t>(handle >> 8);
    out[1] = static_cast<uint8_t>(handle);
    out[2] = static_cast<uint8_t>(route >> 8);
    out[3] = static_cast<uint8_t>(route);
}

int audioRouteMain(const char* verb, const char* routeText, unsigned waitMs) {
    const bool build = !strcmp(verb, "build");
    if (!build) {
        waitMs = waitMs < kReleaseWaitMs ? waitMs : kReleaseWaitMs;  // the service waits for it as it stops
    }
    if (!build && strcmp(verb, "remove")) {
        return 2;
    }
    char* end = nullptr;
    const unsigned long route = strtoul(routeText, &end, 16);
    if (!end || *end || !route || route > 0xffff) {
        return 2;
    }
    Exchange x;
    x.fkt = build ? kBuildHighSyncConnection : kRemoveHighSyncConnection;
    x.handle = static_cast<uint16_t>(kRouteHandleBase | (monoMs() & 0xff));
    if (x.vmost.load() != VmostClient::kLoaded) {
        printf("route 0x%lx: vMOST unavailable (%s)\n", route, x.vmost.error().c_str());
        return 1;
    }
    if (x.vmost.connect(onMessage, &x, 0) != 0) {
        printf("route 0x%lx: vmostd refused the connection\n", route);
        return 1;
    }
    int fd = -1;
    const int rc = x.vmost.shadowRegister(kConnectionMasterFblock, kConnectionMasterInst);
    if (rc != 0 || x.vmost.fileDescriptorGet(&fd) != 0 || fd < 0) {
        printf("route 0x%lx: ConnectionMaster registration refused (%d)\n", route, rc);
        x.vmost.disconnect();
        return 1;
    }
    uint8_t payload[4];
    buildRoutePayload(payload, x.handle, static_cast<uint16_t>(route));
    VmostMsgOut m;
    memset(&m, 0, sizeof(m));
    m.addr = 0xFFFF;
    m.fblock = kConnectionMasterFblock;
    m.inst = kConnectionMasterInst;
    m.fkt = x.fkt;
    m.op = kMostOpStartResultAck;
    m.len = sizeof(payload);
    m.data = payload;
    const uint32_t begin = monoMs();
    if (x.vmost.send(&m) == 0) {
        while (x.answer < 0 && monoMs() - begin < waitMs) {
            struct pollfd p = {fd, POLLIN, 0};
            const int r = poll(&p, 1, 50);
            if (r < 0 && errno != EINTR) {
                break;
            }
            if (r > 0 && (p.revents & POLLIN) && x.vmost.processMessages() != 0) {
                break;
            }
        }
    }
    x.vmost.shadowDeregister(kConnectionMasterFblock, kConnectionMasterInst);
    x.vmost.disconnect();
    const char* what = build ? "connected to the speakers" : "given back";
    if (x.answer == 0xD) {
        printf("route 0x%lx %s by the car's audio manager after %u ms\n", route, what, monoMs() - begin);
        return 0;
    }
    printf("route 0x%lx not %s: %s after %u ms (%u processing answers)\n", route, what,
           x.answer == 0x9 ? "the car refused it" : "no answer", monoMs() - begin, x.processing);
    return 1;
}

void setAudioRouteRequesterForTest(RouteRequester requester) {
    g_requester = requester;
}

void requestAudioRoute(bool build, uint16_t route, bool wait, RouteDone done, void* user) {
    if (g_requester) {
        g_requester(build, route, done, user);
        return;
    }
    Request* r = new Request;
    r->build = build;
    r->route = route;
    r->done = done;
    r->user = user;
    r->queued = !wait;
    r->ticket = 0;
    if (wait) {
        runRequest(r);
        return;
    }
    pthread_mutex_lock(&g_lineMu);
    r->ticket = g_nextTicket++;
    pthread_mutex_unlock(&g_lineMu);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, runRequest, r) != 0) {
        ELOG(kTag, "route 0x%x: no thread to ask for it", route);
        pthread_mutex_lock(&g_lineMu);  // its turn is given up, or the line would stop here
        while (g_serving != r->ticket) {
            pthread_cond_wait(&g_lineCv, &g_lineMu);
        }
        ++g_serving;
        pthread_cond_broadcast(&g_lineCv);
        pthread_mutex_unlock(&g_lineMu);
        if (done) {
            done(false, user);
        }
        delete r;
    }
    pthread_attr_destroy(&attr);
}

}  // namespace evo
