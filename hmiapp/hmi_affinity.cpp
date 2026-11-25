// Copyright (c) 2026 CallMeLewis
// Thread checks for iDrive's per-thread lookups (hmi_affinity.hpp).
#include "hmi_affinity.hpp"

#include <pthread.h>
#include <sys/prctl.h>

#include <atomic>

#include "app_log.hpp"

namespace evo {
namespace app {

namespace {

pthread_t g_main;
std::atomic<bool> g_mainKnown(false);
__thread bool t_inAppTask = false;
const unsigned kLoggedCalls = 16;
std::atomic<const char*> g_logged[kLoggedCalls];

// Whether call was logged before; marks it if not (once per call literal, up to kLoggedCalls of them).
bool loggedBefore(const char* call) {
    for (unsigned i = 0; i < kLoggedCalls; ++i) {
        const char* seen = g_logged[i];
        if (seen == call) {
            return true;
        }
        if (!seen) {
            const char* expected = nullptr;
            if (g_logged[i].compare_exchange_strong(expected, call)) {
                return false;
            }
            if (expected == call) {
                return true;
            }
        }
    }
    return true;  // table full: stay quiet
}

}  // namespace

void rememberHmiMainThread() {
    g_main = pthread_self();
    g_mainKnown = true;
}

bool onHmiMainThread() {
    return g_mainKnown && pthread_equal(pthread_self(), g_main);
}

void enterAppTask(bool inside) {
    t_inAppTask = inside;
}

bool hmiThreadRight(HmiThreadRule rule) {
    const bool main = onHmiMainThread();
    return rule == kHmiMainThread ? main : (main || !t_inAppTask);
}

bool hmiThreadAllowed(const char* call, HmiThreadRule rule) {
    if (hmiThreadRight(rule)) {
        return true;
    }
    if (!loggedBefore(call)) {
        char name[17] = "";
        prctl(PR_GET_NAME, name, 0, 0, 0);
        alog("thread guard: %s on thread '%s', %s: %s", call, name,
             t_inAppTask ? "one of the app's posted tasks" : "not HmiMain3D's own",
             kHmiAffinityEnforced ? "refused" : "made anyway (observe build)");
    }
    return !kHmiAffinityEnforced;
}

}  // namespace app
}  // namespace evo
