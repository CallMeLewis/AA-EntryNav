// Copyright (c) 2026 CallMeLewis
// iDrive's CarPlay player while Android Auto plays sound over Wi-Fi or cable (cluster_player.hpp).
#include "cluster_player.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../platform/service_contract.hpp"
#include "app_log.hpp"
#include "service_link.hpp"

namespace evo {
namespace app {

namespace {

const unsigned kHandlerHead = 0xec;                         // CEntModeButtonService
const unsigned kSkipMapHeader = 0xa4, kSkipMapLeft = 0xac;  // CEntSkipService
const unsigned kMaxNodes = 48;                              // more is a list misread

const int kVtableHeader = 2, kCarPlaySlots = 22, kSlotAvailable = 11;
const int kSkipSlots = 18, kSlotSkip = 1, kSlotPlaying = 4;
const int kSourceCarPlay = 0x82, kClassEntertainment = 1, kSinkAny = 0xff;

typedef void* (*GetCache)();
typedef bool (*AvailableFn)(const void* handler);
typedef int (*CurrentSourceFn)(const void* audio, int audioClass, int sink);
typedef bool (*BuiltUpFn)(const void* audio, int source);
typedef void* (*ClassNameFn)(void* device);
typedef void (*SkipFn)(void* device, int direction);
typedef bool (*PlayingFn)(void* device);
typedef void (*NotifyFn)(void* modes);

void* g_modes = nullptr;  // CEntModeButtonService
void* g_skips = nullptr;  // CEntSkipService
int g_pipe[2] = {-1, -1};
pthread_mutex_t g_peekLock = PTHREAD_MUTEX_INITIALIZER;

void* g_carPlay = nullptr;      // iDrive's CEntModeHandlerCarPlay
void* g_carPlayVptr = nullptr;  // its own vtable pointer, put back on release
void* g_vtableCopy[kVtableHeader + kCarPlaySlots];
AvailableFn g_available = nullptr;
GetCache g_audioGet = nullptr;
void* g_audioVptr = nullptr;  // CHmiAudioService's vtable address point
CurrentSourceFn g_currentSource = nullptr;
BuiltUpFn g_builtUp = nullptr;
unsigned g_asked = 0;
int g_lastAnswer = -1;

void* g_carPlaySkip = nullptr;
void* g_carPlaySkipVptr = nullptr;
void* g_skipVtableCopy[kVtableHeader + kSkipSlots];
ClassNameFn g_className = nullptr;
SkipFn g_skip = nullptr;
PlayingFn g_playing = nullptr;
NotifyFn g_notify = nullptr;
bool g_notifying = false;

int g_toldPid = 0;
int g_toldSkip = -1;

// Copies n bytes from addr in this process, or fails if any of them can't be read: write() checks the source and
// answers EFAULT instead of faulting.
bool peek(const void* addr, void* out, size_t n) {
    if (!addr || g_pipe[0] < 0) {
        return false;
    }
    pthread_mutex_lock(&g_peekLock);
    const bool ok = g_pipe[0] >= 0 && write(g_pipe[1], addr, n) == static_cast<ssize_t>(n) &&
                    read(g_pipe[0], out, n) == static_cast<ssize_t>(n);
    pthread_mutex_unlock(&g_peekLock);
    return ok;
}

uint32_t word(const void* base, unsigned offset, bool* ok) {
    uint32_t v = 0;
    if (!peek(static_cast<const char*>(base) + offset, &v, sizeof(v))) {
        *ok = false;
    }
    return v;
}

void* ptr(const void* base, unsigned offset, bool* ok) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(word(base, offset, ok)));
}

// The class of an object, from the symbol its vtable pointer lands in ("_ZTV22CEntModeHandlerCarPlay+8").
void className(const void* object, char* out, size_t size) {
    bool ok = true;
    void* vtable = ptr(object, 0, &ok);
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (!ok) {
        snprintf(out, size, "(unreadable)");
    } else if (dladdr(vtable, &info) && info.dli_sname) {
        snprintf(out, size, "%s+%ld", info.dli_sname,
                 static_cast<long>(static_cast<const char*>(vtable) - static_cast<const char*>(info.dli_saddr)));
    } else {
        snprintf(out, size, "vtable %p", vtable);
    }
}

void* symbolOf(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

// The service object, from the interface pointer the plugin cache holds. That pointer can be a base other than the
// first (CEntSkipService): its vtable pointer then lands further into the class's vtable group, and the word before
// the typeinfo (offset to top) leads back to the whole object, whose offsets cluster_player.hpp gives.
void* serviceFrom(const char* getter, const char* vtableName) {
    GetCache get = reinterpret_cast<GetCache>(symbolOf(getter));
    void* cache = get ? get() : nullptr;
    bool ok = true;
    void* service = cache ? ptr(cache, 8, &ok) : nullptr;
    void* vptr = service ? ptr(service, 0, &ok) : nullptr;
    Dl_info info;
    memset(&info, 0, sizeof(info));
    const bool known = ok && vptr && dladdr(vptr, &info) && info.dli_sname && strcmp(info.dli_sname, vtableName) == 0;
    int32_t toTop = 0;
    ok = ok && (!known || peek(static_cast<char*>(vptr) - 8, &toTop, sizeof(toTop)));
    void* object = known && ok ? static_cast<char*>(service) + toTop : nullptr;
    if (!object) {
        alog("cluster player: %s not as expected (get %p, interface %p, vtable %s)", vtableName,
             reinterpret_cast<void*>(get), service, info.dli_sname ? info.dli_sname : "?");
    }
    return object;
}

// CarPlay's audio source is iDrive's current entertainment source and built up (CHmiCarPlayService's own test).
bool carPlaySourcePlaying() {
    void* cache = g_audioGet ? g_audioGet() : nullptr;  // iDrive's thread: its plugin cache answers
    void* audio = cache ? *reinterpret_cast<void**>(static_cast<char*>(cache) + 8) : nullptr;
    if (!audio || *static_cast<void**>(audio) != g_audioVptr) {
        return false;
    }
    return g_currentSource(audio, kClassEntertainment, kSinkAny) == kSourceCarPlay && g_builtUp(audio, kSourceCarPlay);
}

bool carPlayAvailable(const void* handler) {
    const bool own = g_available(handler);
    const bool answer = own || (handler == g_carPlay && carPlaySourcePlaying());
    ++g_asked;
    if (static_cast<int>(answer) != g_lastAnswer) {
        alog("cluster player: iDrive asks whether CarPlay's player is available (%u time(s)): %s", g_asked,
             own      ? "yes, iDrive's own answer"
             : answer ? "yes, CarPlay's source plays (Android Auto)"
                      : "no");
        g_lastAnswer = answer;
    }
    return answer;
}

bool carPlaySkipPlaying(void* device) {
    return g_playing(device) || (device == g_carPlaySkip && carPlaySourcePlaying());
}

void carPlaySkip(void* device, int direction) {
    if (g_playing(device) || device != g_carPlaySkip || !carPlaySourcePlaying()) {
        g_skip(device, direction);
        return;
    }
    sendToService(direction > 0 ? "skip 1" : "skip -1");
    alog("cluster player: iDrive's cluster menu: %s track, sent to the service", direction > 0 ? "next" : "previous");
}

// iDrive rebuilds the mode event when its players change, not when an audio source is built up. As Android Auto's
// route comes up, the skip service makes CarPlay's skip device (source 0x82) current and calls slot 0
// (getSkipDeviceClassName) for its log; when source 0x82's state differs from what the player last answered, we
// trigger notifyModeSequenceListeners so the cluster refreshes its menu.
void* carPlaySkipClassName(void* device) {
    void* name = g_className(device);
    if (device == g_carPlaySkip && !g_notifying && g_lastAnswer != -1 &&
        carPlaySourcePlaying() != (g_lastAnswer == 1)) {
        g_notifying = true;
        alog("cluster player: CarPlay's skip device is in use and its source %s: the players' event is rebuilt",
             g_lastAnswer == 1 ? "stopped" : "plays");
        g_notify(g_modes);
        g_notifying = false;
    }
    return name;
}

void installSkipNudge() {
    void** vtable = static_cast<void**>(symbolOf("_ZTV29CDevExternalDevicesSkipDevice"));
    void* classNameFn = symbolOf("_ZN29CDevExternalDevicesSkipDevice22getSkipDeviceClassNameEv");
    g_notify = reinterpret_cast<NotifyFn>(symbolOf("_ZN21CEntModeButtonService27notifyModeSequenceListenersEv"));
    bool ok = true;
    const char* header = g_skips ? static_cast<const char*>(g_skips) + kSkipMapHeader : nullptr;
    void* node = g_skips ? ptr(g_skips, kSkipMapLeft, &ok) : nullptr;
    void* device = nullptr;
    for (unsigned i = 0; node && node != header && ok && i < kMaxNodes && !device; ++i) {
        if (word(node, 0x10, &ok) == static_cast<uint32_t>(kSourceCarPlay) && word(node, 0x14, &ok) == 0) {
            device = ptr(node, 0x18, &ok);
            break;
        }
        void* right = ptr(node, 0xc, &ok);
        if (right) {
            node = right;
            for (void* left = ptr(node, 8, &ok); left && ok; left = ptr(node, 8, &ok)) {
                node = left;
            }
        } else {
            void* parent = ptr(node, 4, &ok);
            while (ok && parent && parent != header && ptr(parent, 0xc, &ok) == node) {
                node = parent;
                parent = ptr(node, 4, &ok);
            }
            node = parent;
        }
    }
    const bool known = ok && device && vtable && ptr(device, 0, &ok) == static_cast<void*>(vtable + kVtableHeader) &&
                       classNameFn && vtable[kVtableHeader] == classNameFn && g_notify;
    if (!known) {
        alog("cluster player: no nudge (skip device for 0x%x %p, its class not as expected)", kSourceCarPlay, device);
        return;
    }
    memcpy(g_skipVtableCopy, vtable, sizeof(g_skipVtableCopy));
    g_className = reinterpret_cast<ClassNameFn>(classNameFn);
    g_skip = reinterpret_cast<SkipFn>(vtable[kVtableHeader + kSlotSkip]);
    g_playing = reinterpret_cast<PlayingFn>(vtable[kVtableHeader + kSlotPlaying]);
    g_skipVtableCopy[kVtableHeader] = reinterpret_cast<void*>(&carPlaySkipClassName);
    g_skipVtableCopy[kVtableHeader + kSlotSkip] = reinterpret_cast<void*>(&carPlaySkip);
    g_skipVtableCopy[kVtableHeader + kSlotPlaying] = reinterpret_cast<void*>(&carPlaySkipPlaying);
    g_carPlaySkip = device;
    g_carPlaySkipVptr = *static_cast<void**>(device);
    *static_cast<void**>(device) = &g_skipVtableCopy[kVtableHeader];
    alog("cluster player: nudge installed on CarPlay's skip device %p", device);
}

void installClusterPlayer() {
    void** vtable = static_cast<void**>(symbolOf("_ZTV22CEntModeHandlerCarPlay"));
    void* availableFn = symbolOf("_ZNK19CEntModeHandlerBase22isModeHandlerAvailableEv");
    void** audioVtable = static_cast<void**>(symbolOf("_ZTV16CHmiAudioService"));
    g_audioGet = reinterpret_cast<GetCache>(
        symbolOf("_ZN9hmiplugin3getI16IHmiAudioServiceEENS_25CPluginInstanceCacheUsageIT_EEv"));
    g_currentSource = reinterpret_cast<CurrentSourceFn>(
        symbolOf("_ZNK16CHmiAudioService21getCurrentAudioSourceEN8HmiAudio11EAudioClassENS0_10EAudioSinkE"));
    g_builtUp = reinterpret_cast<BuiltUpFn>(symbolOf("_ZNK16CHmiAudioService20isAudioSourceBuiltUpE12EAudioSource"));
    void* cache = g_audioGet ? g_audioGet() : nullptr;
    void* audio = cache ? *reinterpret_cast<void**>(static_cast<char*>(cache) + 8) : nullptr;
    g_audioVptr = audioVtable ? static_cast<void*>(audioVtable + kVtableHeader) : nullptr;
    const bool audioKnown =
        audio && g_audioVptr && *static_cast<void**>(audio) == g_audioVptr && g_currentSource && g_builtUp;
    bool ok = true;
    void* node = g_modes ? ptr(g_modes, kHandlerHead, &ok) : nullptr;
    for (unsigned i = 0; node && ok && i < kMaxNodes && !g_carPlay; ++i) {
        void* handler = ptr(node, 8, &ok);
        if (ok && vtable && ptr(handler, 0, &ok) == static_cast<void*>(vtable + kVtableHeader)) {
            g_carPlay = handler;
        }
        node = ptr(node, 0, &ok);
    }
    const bool slotKnown = vtable && availableFn && vtable[kVtableHeader + kSlotAvailable] == availableFn;
    if (!g_carPlay || !slotKnown || !audioKnown) {
        alog("cluster player: not installed (CarPlay's player %p, its slot 11 %s, audio service %p %s)", g_carPlay,
             slotKnown ? "as expected" : "not isModeHandlerAvailable", audio, audioKnown ? "as expected" : "not known");
        g_carPlay = nullptr;
        return;
    }
    memcpy(g_vtableCopy, vtable, sizeof(g_vtableCopy));
    g_available = reinterpret_cast<AvailableFn>(availableFn);
    g_vtableCopy[kVtableHeader + kSlotAvailable] = reinterpret_cast<void*>(&carPlayAvailable);
    g_carPlayVptr = *static_cast<void**>(g_carPlay);
    *static_cast<void**>(g_carPlay) = &g_vtableCopy[kVtableHeader];  // one aligned word: iDrive sees one or the other
    alog("cluster player: installed on CarPlay's player %p (available while source 0x%x plays, now %s)", g_carPlay,
         kSourceCarPlay, carPlaySourcePlaying() ? "playing" : "not playing");
    installSkipNudge();
}

}  // namespace

bool clusterPlayerPeek(const void* addr, void* out, size_t n) {
    return peek(addr, out, n);
}

void clusterPlayerClassName(const void* object, char* out, size_t size) {
    className(object, out, size);
}

bool clusterPlayerInstalled() {
    return g_carPlay != nullptr;
}

void releaseClusterPlayer() {
    if (g_carPlay && *static_cast<void**>(g_carPlay) == &g_vtableCopy[kVtableHeader]) {
        *static_cast<void**>(g_carPlay) = g_carPlayVptr;
        alog("cluster player: CarPlay's player has its own vtable back");
    }
    if (g_carPlaySkip && *static_cast<void**>(g_carPlaySkip) == &g_skipVtableCopy[kVtableHeader]) {
        *static_cast<void**>(g_carPlaySkip) = g_carPlaySkipVptr;
        alog("cluster player: CarPlay's skip device has its own vtable back");
    }
    g_carPlay = nullptr;
    g_carPlaySkip = nullptr;
    g_modes = nullptr;
    g_skips = nullptr;
    pthread_mutex_lock(&g_peekLock);
    if (g_pipe[0] >= 0) {
        close(g_pipe[0]);
        g_pipe[0] = -1;
    }
    if (g_pipe[1] >= 0) {
        close(g_pipe[1]);
        g_pipe[1] = -1;
    }
    pthread_mutex_unlock(&g_peekLock);
}

void captureClusterPlayer() {
    if (access(contract::kAudioWifi, F_OK) != 0) {
        return;
    }
    if (pipe(g_pipe) != 0) {
        alog("cluster player: no pipe: not installed");
        return;
    }
    g_modes = serviceFrom("_ZN9hmiplugin3getI21IEntModeButtonServiceEENS_25CPluginInstanceCacheUsageIT_EEv",
                          "_ZTV21CEntModeButtonService");
    g_skips = serviceFrom("_ZN9hmiplugin3getI15IEntSkipServiceEENS_25CPluginInstanceCacheUsageIT_EEv",
                          "_ZTV15CEntSkipService");
    installClusterPlayer();
}

void clusterPlayerPoll(int servicePid, bool connected) {
    if (!g_carPlaySkip) {
        return;
    }
    if (!connected || servicePid <= 0) {
        g_toldPid = 0;
        g_toldSkip = -1;
        return;
    }
    const int skip = g_lastAnswer == 1 ? 1 : 0;
    if (servicePid != g_toldPid || skip != g_toldSkip) {
        sendToService(skip ? "clusterskip 1" : "clusterskip 0");
        g_toldPid = servicePid;
        g_toldSkip = skip;
    }
}

}  // namespace app
}  // namespace evo
