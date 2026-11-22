// Copyright (c) 2026 CallMeLewis
// The phone's track title on the cluster's CarPlay menu (cluster_title.hpp).
#include "cluster_title.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "../platform/service_contract.hpp"
#include "app_log.hpp"
#include "ent_probe.hpp"
#include "hmi_thread.hpp"
#include "utf16.hpp"

namespace evo {
namespace app {

namespace {

const unsigned char kCarPlayInstance = 0x70;
const unsigned kFileInfo = 0x38c0;                          // the CAuxIn_CurrentAudioFileInfo in the proxy
const unsigned kMask = 0x7a, kData = 0x8c, kTitle = 0x2e0;  // in it, and in its CPRAudioMediaData
const unsigned kDelegateA = 0x4c, kDelegateB = 0x6c;
const size_t kMaxUnits = 200;
const long kReapplyS = 5;

typedef void* (*GetProxy)(unsigned char instance);
typedef void* (*StringFromUnits)(void* self, const uint16_t* units, int count);
typedef void* (*AssignString)(void* self, const void* other);
typedef void (*DestroyString)(void* self);
typedef void (*SetValid)(void* proxy, bool valid);
typedef void (*Invoke)(void* delegate, void* proxy);

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
uint16_t g_units[kMaxUnits];  // the title to show, under g_lock
size_t g_count = 0;
std::string g_lastFile;  // status poll only
long g_lastAskS = 0;
bool g_wrote = false;  // the cluster's thread only: a title of ours is in the proxy

void* symbol(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

// The first line of the track file, the title; empty without a file (no session).
std::string readTitle() {
    FILE* f = fopen(contract::kTrackFile, "r");
    if (!f) {
        return std::string();
    }
    char line[512] = "";
    const bool got = fgets(line, sizeof(line), f) != nullptr;
    fclose(f);
    if (!got) {
        return std::string();
    }
    line[strcspn(line, "\r\n")] = 0;
    return line;
}

// The THmiString<unsigned short> holds a pointer to its units; its length is the int 12 bytes before them.
bool sameText(const void* hmiString, const uint16_t* units, size_t count) {
    const uint16_t* text = *static_cast<const uint16_t* const*>(hmiString);
    if (!text) {
        return count == 0;
    }
    const int length = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(text) - 12);
    return length >= 0 && static_cast<size_t>(length) == count && memcmp(text, units, count * 2) == 0;
}

// Diagnostic (temporary): what listens to the proxy's two delegates, and the state of any skip controller among them.
// The menu draws the title only while its +0x69 (active, set by onModeActivate) is set; it reads the proxy it took
// from getServiceProxy(+0x68) into +0x50, and writes the label at +0x54 and the button at +0x60.
int g_diagLeft = 0;
unsigned g_diagPeeks = 0;

void logObject(const void* where, const char* path) {
    char name[160];
    entProbeClassName(where, name, sizeof(name));
    if (strncmp(name, "_ZTV", 4) != 0) {
        return;
    }
    alog("cluster title diag: %s -> %p %s", path, where, name);
    if (strstr(name, "SkipController")) {
        // A delegate's target can be a base inside the object: offset to top, before the typeinfo, leads to its start.
        uint32_t vptr = 0;
        int32_t toTop = 0;
        entProbePeek(where, &vptr, 4);
        entProbePeek(reinterpret_cast<const char*>(static_cast<uintptr_t>(vptr)) - 8, &toTop, 4);
        const char* c = static_cast<const char*>(where) + toTop;
        uint32_t w[4] = {0, 0, 0, 0};
        uint8_t b[2] = {0, 0};
        entProbePeek(c + 0x50, &w[0], 4);
        entProbePeek(c + 0x54, &w[1], 4);
        entProbePeek(c + 0x60, &w[2], 4);
        entProbePeek(c + 0x68, b, 2);
        char label[160] = "";
        entProbeClassName(reinterpret_cast<void*>(static_cast<uintptr_t>(w[1])), label, sizeof(label));
        alog(
            "cluster title diag:   controller %p proxy 0x%08x label 0x%08x (%s) button 0x%08x instance 0x%02x active "
            "%u",
            static_cast<const void*>(c), w[0], w[1], label, w[2], b[0], b[1]);
        // Once: the menu's own refresh, called directly on this thread, if this is the CarPlay menu.
        typedef void (*Update)(const void* controller);
        static void** carPlayVtable = static_cast<void**>(symbol("_ZTV24CICCarPlaySkipController"));
        static Update update = reinterpret_cast<Update>(symbol("_ZN20CICUSBSkipController18updateTitleDetailsEv"));
        static bool called = false;
        uint32_t own = 0;
        entProbePeek(c, &own, 4);
        if (!called && carPlayVtable && update &&
            own == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(carPlayVtable + 2))) {
            called = true;
            update(c);
            alog("cluster title diag:   called updateTitleDetails on the CarPlay menu");
        }
    }
}

void scan(const void* base, unsigned size, int depth, const char* path) {
    for (unsigned o = 0; o < size && g_diagPeeks < 400; o += 4) {
        uint32_t w = 0;
        ++g_diagPeeks;
        if (!entProbePeek(static_cast<const char*>(base) + o, &w, 4) || w < 0x10000 || (w & 3)) {
            continue;
        }
        const void* p = reinterpret_cast<const void*>(static_cast<uintptr_t>(w));
        char here[96];
        snprintf(here, sizeof(here), "%s+0x%x", path, o);
        logObject(p, here);
        if (depth > 0) {
            scan(p, 0x20, depth - 1, here);
        }
    }
}

void logListeners(char* info) {
    typedef bool (*IsValid)(const void* proxy);
    static IsValid isValid = reinterpret_cast<IsValid>(symbol("_ZNK17CSvcFunctionProxy7isValidEv"));
    const uint32_t* w = reinterpret_cast<const uint32_t*>(info + 0x40);
    alog("cluster title diag: valid %d, +0x40: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
         isValid ? static_cast<int>(isValid(info)) : -1, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9],
         w[10], w[11]);
    g_diagPeeks = 0;
    scan(info + 0x40, 0x4c, 2, "info+0x40");
}

}  // namespace

void clusterTitlePoll(long nowS) {
    if (!clusterPlayerInstalled()) {
        return;
    }
    const std::string title = readTitle();
    const bool changed = title != g_lastFile;
    if (!changed && nowS - g_lastAskS < kReapplyS) {
        return;
    }
    if (changed) {
        uint16_t units[kMaxUnits];
        const size_t count = utf8ToUtf16(title.data(), title.size(), units, kMaxUnits);
        pthread_mutex_lock(&g_lock);
        memcpy(g_units, units, count * 2);
        g_count = count;
        pthread_mutex_unlock(&g_lock);
        g_lastFile = title;
        alog("cluster title: %s", title.empty() ? "none (no track)" : title.c_str());
    }
    g_lastAskS = nowS;
    requestHmiThread();
}

void clusterTitleApply() {
    if (!clusterPlayerInstalled()) {
        return;
    }
    static GetProxy getProxy = reinterpret_cast<GetProxy>(symbol("_ZN18CPxyAuxiliaryInput15getServiceProxyEh"));
    static void** vtable = static_cast<void**>(symbol("_ZTVN18CPxyAuxiliaryInput27CAuxIn_CurrentAudioFileInfoE"));
    static StringFromUnits fromUnits = reinterpret_cast<StringFromUnits>(symbol("_ZN10THmiStringItEC1EPKti"));
    static AssignString assign = reinterpret_cast<AssignString>(symbol("_ZN10THmiStringItEaSERKS0_"));
    static DestroyString destroy = reinterpret_cast<DestroyString>(symbol("_ZN10THmiStringItED1Ev"));
    static SetValid setValid = reinterpret_cast<SetValid>(symbol("_ZN17CSvcFunctionProxy8setValidEb"));
    static Invoke invoke = reinterpret_cast<Invoke>(symbol("_ZN9TDelegateIFvP17CSvcFunctionProxyEE6invokeES1_"));
    static bool reported = false;
    char* proxy = getProxy ? static_cast<char*>(getProxy(kCarPlayInstance)) : nullptr;
    char* info = proxy ? proxy + kFileInfo : nullptr;
    const bool known = info && vtable && *reinterpret_cast<void**>(info) == static_cast<void*>(vtable + 2) &&
                       fromUnits && assign && destroy && setValid && invoke;
    if (!known) {
        if (!reported) {
            alog("cluster title: CarPlay's AuxIn proxy not as expected (proxy %p): no title",
                 static_cast<void*>(proxy));
            reported = true;
        }
        return;
    }
    uint16_t units[kMaxUnits];
    pthread_mutex_lock(&g_lock);
    const size_t count = g_count;
    memcpy(units, g_units, count * 2);
    pthread_mutex_unlock(&g_lock);
    void* title = info + kData + kTitle;
    if ((count == 0 && !g_wrote) || sameText(title, units, count)) {
        if (g_diagLeft > 0) {
            --g_diagLeft;
            logListeners(info);
        }
        return;  // nothing of ours to clear, or the title is already there
    }
    uint32_t made = 0;  // a THmiString is one pointer
    fromUnits(&made, units, static_cast<int>(count));
    assign(title, &made);
    destroy(&made);
    info[kMask] = static_cast<char>(info[kMask] | 0x02);  // the title is in this status
    setValid(info, true);
    invoke(info + kDelegateA, info);
    invoke(info + kDelegateB, info);
    g_wrote = count != 0;
    alog("cluster title: %u character(s) put in CarPlay's current audio file info", static_cast<unsigned>(count));
    logListeners(info);
    g_diagLeft = 2;
}

}  // namespace app
}  // namespace evo
