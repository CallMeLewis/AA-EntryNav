// Copyright (c) 2026 CallMeLewis
// The iDrive app's thread guard (hmiapp/hmi_affinity.hpp): which threads may make iDrive's per-thread lookups, and one
// log line per wrong-thread lookup.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "../hmiapp/hmi_affinity.hpp"

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)

static std::vector<std::string> logged;

namespace evo {
namespace app {
void alog(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    logged.push_back(line);
}
}  // namespace app
}  // namespace evo

using namespace evo::app;

// A bmw::Dispatcher thread: outside a posted task, then inside one.
static void* dispatcher(void*) {
    CHECK(!onHmiMainThread());
    CHECK(!hmiThreadRight(kHmiMainThread) && hmiThreadRight(kHmiUnitThread));  // the cluster's unit, say
    enterAppTask(true);
    CHECK(!hmiThreadRight(kHmiUnitThread) && !hmiThreadRight(kHmiMainThread));
    const char* const call = "hmiplugin::get<IHmiAudioService>";
    CHECK(hmiThreadAllowed(call, kHmiUnitThread) == !kHmiAffinityEnforced);
    CHECK(logged.size() == 1 && logged[0].find(call) != std::string::npos &&
          logged[0].find("posted tasks") != std::string::npos);
    CHECK(hmiThreadAllowed(call, kHmiUnitThread) == !kHmiAffinityEnforced && logged.size() == 1);  // once per call
    CHECK(hmiThreadAllowed("getHmiProcUnit", kHmiMainThread) == !kHmiAffinityEnforced && logged.size() == 2);
    enterAppTask(false);
    CHECK(hmiThreadRight(kHmiUnitThread));
    return nullptr;
}

int main() {
    CHECK(!hmiThreadRight(kHmiMainThread));  // before createApp: no thread is HmiMain3D's yet
    rememberHmiMainThread();
    CHECK(onHmiMainThread() && hmiThreadRight(kHmiMainThread) && hmiThreadRight(kHmiUnitThread));
    CHECK(hmiThreadAllowed("getHmiProcUnit", kHmiMainThread) && logged.empty());
    pthread_t t;
    CHECK(pthread_create(&t, nullptr, dispatcher, nullptr) == 0 && pthread_join(t, nullptr) == 0);
    enterAppTask(true);  // HmiMain3D's own thread may run a posted task too: still right
    CHECK(hmiThreadRight(kHmiUnitThread) && hmiThreadRight(kHmiMainThread));
    puts(
        "PASS iDrive app thread guard: HmiMain3D's own thread for the createApp lookups, any HMI thread but a posted "
        "task's for the hooks, one log line per wrong-thread lookup, observe build makes it anyway");
    return 0;
}
