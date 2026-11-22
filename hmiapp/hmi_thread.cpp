// Copyright (c) 2026 CallMeLewis
// Work for the instrument cluster's own HMI thread (hmi_thread.hpp).
#include "hmi_thread.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "app_log.hpp"
#include "ent_probe.hpp"

namespace evo {
namespace app {

namespace {

const int kSlotSendEvent = 5, kSlotRegisterEvent = 9;  // IHmiProcUnit (bt_audio.hpp, CHmiProcUnit's vtable)
const unsigned short kRunId = 1;
const char kGroupName[] = "AAEntryNav";
// CHmiProcUnit (libhmicom.so): the main dispatcher it hands events to. CHmiMainDispatcher: its std::map from an
// event's key to the std::vector<IHmiProcUnit*> registered for it, header at +4 and leftmost node at +0xc; a node's
// vector at +0x14 (begin) and +0x18 (end).
const unsigned kUnitMainDispatcher = 0x33c;
const unsigned kMapHeader = 4, kMapLeftmost = 0xc, kNodeVectorBegin = 0x14;
const unsigned kMaxNodes = 4000, kMaxUnitsPerKey = 64;
const char kClusterUnitClass[] = "_ZTV14CHmiICProcUnit+8";
const unsigned kMaxTries = 120;  // one a second from the status poll

typedef void* (*GetHmiProcUnit)();
typedef GetHmiProcUnit GetHmiProcUnitFn;
typedef unsigned short (*RegisterGroup)(const char* name);
// The member function pointer goes as two words, its function then its adjustment.
typedef void (*RegisterEvent)(void* queue, void* target, void* fn, int adj, unsigned short group, unsigned short id);
typedef void* (*CreateEvent)(unsigned);
typedef bool (*SendEvent)(void* queue, void* event);

void* g_main = nullptr;  // HmiMain3D's own proc unit: events are sent through it
void* g_registerFn = nullptr;
void* g_sendFn = nullptr;
std::atomic<bool> g_registered(false);
unsigned g_tries = 0;
unsigned short g_group = 0;
CreateEvent g_create = nullptr;
HmiThreadJob g_job = nullptr;
uint32_t g_key = 0;
pthread_t g_thread;
std::atomic<bool> g_threadKnown(false);
std::atomic<bool> g_pending(false);
std::atomic<unsigned> g_runs(0);
char g_target;  // the listener's target: nothing is read from it

void* g_cluster = nullptr;  // the cluster's proc unit, once found
GetHmiProcUnitFn g_getUnit = nullptr;
std::atomic<unsigned> g_clusterRuns(0);

// iDrive's dispatcher calls this as a member function of g_target, on each listening unit's thread: HmiMain3D's own
// and the cluster's.
void onEventEntry(void* /*target*/, const void* /*event*/) {
    g_pending = false;
    const bool cluster = g_getUnit && g_cluster && g_getUnit() == g_cluster;
    if (cluster && g_clusterRuns++ == 0) {
        g_thread = pthread_self();
        g_threadKnown = true;
        alog("hmi thread: first job run on the cluster's thread");
    }
    if (!cluster && g_runs++ == 0) {
        alog("hmi thread: first job run on HmiMain3D's own thread");
    }
    if (g_job) {
        g_job();
    }
}

void* symbol(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

uint32_t word(const void* base, unsigned offset, bool* ok) {
    uint32_t v = 0;
    if (!entProbePeek(static_cast<const char*>(base) + offset, &v, sizeof(v))) {
        *ok = false;
    }
    return v;
}

void* at(uint32_t v) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(v));
}

bool slotsKnown(const void* unit) {
    bool ok = true;
    void* vtable = at(word(unit, 0, &ok));
    return ok && vtable && at(word(vtable, kSlotRegisterEvent * 4, &ok)) == g_registerFn &&
           at(word(vtable, kSlotSendEvent * 4, &ok)) == g_sendFn && ok;
}

// The next node of a std::map, as _Rb_tree_increment: node {colour, parent, left, right}.
void* nextNode(void* node, bool* ok) {
    void* right = at(word(node, 0xc, ok));
    if (right) {
        node = right;
        for (void* left = at(word(node, 8, ok)); left && *ok; left = at(word(node, 8, ok))) {
            node = left;
        }
        return node;
    }
    void* parent = at(word(node, 4, ok));
    while (*ok && node == at(word(parent, 0xc, ok))) {
        node = parent;
        parent = at(word(node, 4, ok));
    }
    // The header's right child can be the node itself in a one-node tree; std's increment checks the same.
    if (*ok && at(word(node, 0xc, ok)) != parent) {
        node = parent;
    }
    return node;
}

// The cluster's proc unit, from the main dispatcher's routing map: the unit of class CHmiICProcUnit registered for
// some event. Read without the dispatcher's lock, every read failing safely, so a moving map is a miss to try again.
void* findClusterUnit() {
    bool ok = true;
    void* dispatcher = at(word(g_main, kUnitMainDispatcher, &ok));
    if (!ok || !dispatcher) {
        return nullptr;
    }
    const char* header = static_cast<const char*>(dispatcher) + kMapHeader;
    void* node = at(word(dispatcher, kMapLeftmost, &ok));
    for (unsigned n = 0; ok && node && node != header && n < kMaxNodes; ++n) {
        const uint32_t begin = word(node, kNodeVectorBegin, &ok), end = word(node, kNodeVectorBegin + 4, &ok);
        for (uint32_t p = begin; ok && p < end && p - begin < kMaxUnitsPerKey * 4; p += 4) {
            void* unit = at(word(at(p), 0, &ok));
            char name[96];
            if (ok && unit && unit != g_main) {
                entProbeClassName(unit, name, sizeof(name));
                if (strcmp(name, kClusterUnitClass) == 0) {
                    return unit;
                }
            }
        }
        node = nextNode(node, &ok);
    }
    return nullptr;
}

// Puts the listener on the cluster's unit: doRegisterEvent takes that unit's dispatcher lock, the one its thread
// holds while dispatching, and tells the main dispatcher to route the app's key there.
bool registerOnCluster() {
    if (g_registered) {
        return true;
    }
    if (g_tries >= kMaxTries) {
        return false;
    }
    ++g_tries;
    void* unit = findClusterUnit();
    if (!unit || !slotsKnown(unit)) {
        if (g_tries == 1 || g_tries == kMaxTries) {
            alog("hmi thread: cluster unit %s (try %u)", unit ? "not as expected" : "not found yet", g_tries);
        }
        return false;
    }
    g_cluster = unit;
    reinterpret_cast<RegisterEvent>(g_registerFn)(unit, &g_target, reinterpret_cast<void*>(&onEventEntry), 0, g_group,
                                                  kRunId);
    g_registered = true;
    alog("hmi thread: listening on the cluster's unit %p (try %u)", unit, g_tries);
    return true;
}

}  // namespace

bool captureHmiThread(HmiThreadJob job) {
    GetHmiProcUnit get = reinterpret_cast<GetHmiProcUnit>(symbol("_Z14getHmiProcUnitv"));
    RegisterGroup group = reinterpret_cast<RegisterGroup>(symbol("_ZN16CHmiDispatchInfo20registerEventGroupIDEPKc"));
    g_registerFn = symbol("_ZN12CHmiProcUnit15doRegisterEventEPvM15CDelegateTargetFvPK9CHmiEventEtt");
    g_sendFn = symbol("_ZN12CHmiProcUnit9sendEventEP9CHmiEvent");
    g_create = reinterpret_cast<CreateEvent>(symbol("_ZN25CHmiModePlayerSelectEvent11createEventEj"));
    void* queue = get ? get() : nullptr;
    // The slots must be the functions read: another iDrive build may lay the vtable out differently.
    const bool known = queue && g_registerFn && g_sendFn && slotsKnown(queue) && group && g_create;
    if (!known) {
        alog("hmi thread: not available (queue %p, registerEventGroupID %s, slots %s, createEvent %s)", queue,
             group ? "found" : "missing", queue && slotsKnown(queue) ? "as expected" : "not as expected",
             g_create ? "found" : "missing");
        return false;
    }
    g_group = group(kGroupName);
    if (!g_group) {
        alog("hmi thread: iDrive gave no event group for %s", kGroupName);
        return false;
    }
    g_job = job;
    g_key = static_cast<uint32_t>(g_group) << 16 | kRunId;
    g_main = queue;
    g_getUnit = get;
    // HmiMain3D's own unit listens too: the main menu and the media screens read its proxies.
    reinterpret_cast<RegisterEvent>(g_registerFn)(queue, &g_target, reinterpret_cast<void*>(&onEventEntry), 0, g_group,
                                                  kRunId);
    alog("hmi thread: event group 0x%04x (%s), listening on HmiMain3D's queue %p", g_group, kGroupName, queue);
    registerOnCluster();
    return true;
}

bool requestHmiThread() {
    if (!g_main) {
        return false;
    }
    registerOnCluster();  // until found: HmiMain3D's own unit listens meanwhile
    if (g_pending.exchange(true)) {
        return true;  // one already queued: it does this work too
    }
    void* event = g_create(0);
    if (!event) {
        g_pending = false;
        return false;
    }
    *reinterpret_cast<uint32_t*>(static_cast<char*>(event) + 4) = g_key;
    // The main dispatcher routes it to the cluster's unit and owns it from here, as with bt_audio.cpp's popup.
    reinterpret_cast<SendEvent>(g_sendFn)(g_main, event);
    return true;
}

bool onHmiThread() {
    return g_threadKnown && pthread_equal(pthread_self(), g_thread);  // the cluster's thread
}

}  // namespace app
}  // namespace evo
