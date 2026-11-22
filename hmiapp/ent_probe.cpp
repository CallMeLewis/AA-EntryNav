// Copyright (c) 2026 CallMeLewis
// The read-only look at iDrive's media players and skip devices (ent_probe.hpp).
#include "ent_probe.hpp"

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

const unsigned kHandlerHead = 0xec, kHandlerCurrent = 0xe8;                      // CEntModeButtonService
const unsigned kSkipCurrent = 0x40, kSkipMapHeader = 0xa4, kSkipMapLeft = 0xac;  // CEntSkipService
const unsigned kMaxNodes = 48;                                                   // more is a list misread

typedef void* (*GetCache)();

void* g_modes = nullptr;  // CEntModeButtonService
void* g_skips = nullptr;  // CEntSkipService
int g_pipe[2] = {-1, -1};
pthread_mutex_t g_peekLock = PTHREAD_MUTEX_INITIALIZER;
uint32_t g_lastDigest = 0;
long g_lastLogS = 0;

// Copies n bytes from addr in this process, or fails if any of them can't be read: write() checks the source and
// answers EFAULT instead of faulting.
bool peek(const void* addr, void* out, size_t n) {
    if (!addr || g_pipe[0] < 0) {
        return false;
    }
    pthread_mutex_lock(&g_peekLock);
    const bool ok =
        write(g_pipe[1], addr, n) == static_cast<ssize_t>(n) && read(g_pipe[0], out, n) == static_cast<ssize_t>(n);
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

// The service object, from the interface pointer the plugin cache holds. That pointer can be a base other than the
// first (CEntSkipService): its vtable pointer then lands further into the class's vtable group, and the word before
// the typeinfo (offset to top) leads back to the whole object, whose offsets ent_probe.hpp gives.
void* serviceFrom(const char* getter, const char* vtableName) {
    GetCache get = reinterpret_cast<GetCache>(dlsym(RTLD_DEFAULT, getter));
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
    alog("ent probe: %s: get %p, cache %p, interface %p, vtable %s+%ld, object %p", vtableName,
         reinterpret_cast<void*>(get), cache, service, info.dli_sname ? info.dli_sname : "?",
         static_cast<long>(static_cast<char*>(vptr) - static_cast<char*>(info.dli_saddr)), object);
    return object;
}

// The handler list, or a digest of what it holds without logging (log false).
uint32_t walkHandlers(bool log) {
    if (!g_modes) {
        return 0;
    }
    bool ok = true;
    void* current = ptr(g_modes, kHandlerCurrent, &ok);
    void* node = ptr(g_modes, kHandlerHead, &ok);
    uint32_t digest = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(current));
    unsigned count = 0;
    for (; node && ok && count < kMaxNodes; ++count) {
        void* handler = ptr(node, 8, &ok);
        const uint32_t position = word(node, 0xc, &ok), type = word(node, 0x10, &ok);
        digest = digest * 31 + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handler)) + type;
        if (log) {
            char name[96];
            className(handler, name, sizeof(name));
            alog("ent probe: player %u%s: handler %p %s, position %u, player type 0x%x", count,
                 node == current ? " (current)" : "", handler, name, position, type);
        }
        node = ptr(node, 0, &ok);
    }
    if (log) {
        alog("ent probe: %u player(s) registered, current node %p%s", count, current, ok ? "" : ", a read failed");
    }
    return digest + count;
}

// The skip devices by audio source, and the current one.
uint32_t walkSkips(bool log) {
    if (!g_skips) {
        return 0;
    }
    bool ok = true;
    void* current = ptr(g_skips, kSkipCurrent, &ok);
    const char* header = static_cast<const char*>(g_skips) + kSkipMapHeader;
    void* node = ptr(g_skips, kSkipMapLeft, &ok);
    uint32_t digest = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(current));
    unsigned count = 0;
    while (node && node != header && ok && count < kMaxNodes) {
        const uint32_t source = word(node, 0x10, &ok), waveband = word(node, 0x14, &ok);
        void* device = ptr(node, 0x18, &ok);
        digest = digest * 31 + source + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(device));
        if (log) {
            char name[96];
            className(device, name, sizeof(name));
            alog("ent probe: skip device for source 0x%x waveband %u: %p %s%s", source, waveband, device, name,
                 device == current ? " (current)" : "");
        }
        ++count;
        // In-order successor in the red-black tree: node +4 parent, +8 left, +0xc right.
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
    if (log) {
        char name[96];
        className(current, name, sizeof(name));
        alog("ent probe: %u skip device(s), current %p %s%s", count, current, current ? name : "",
             ok ? "" : ", a read failed");
    }
    return digest + count;
}

void logAll(const char* why) {
    alog("ent probe: --- %s", why);
    walkHandlers(true);
    walkSkips(true);
}

// ---- the cluster player experiment (kClusterPlayer) -----------------------------------------------------------------
// iDrive's CarPlay player (CEntModeHandlerCarPlay) stays registered and current while Android Auto's sound plays on
// CarPlay's audio source, 0x82, but counts as unavailable without an iPhone (its device is not connected). Building
// the mode event, CEntModeButtonService::notifyModeSequenceListeners then marks the current player not active, and the
// cluster maps CarPlay not active to CDMM_NO_AUDIO: "no active entertainment source". Asked "available" through its
// vtable slot 11 (+0x2c, isModeHandlerAvailable), the handler would be marked active (getPendingLUMSource is 0), and
// the cluster takes CarPlay's mode (CICEntertainmentSourcesListController::PlayerTypeToICMode, type 0x1e).
// So this one object gets a copy of its vtable whose slot 11 answers iDrive's own function, or yes while source 0x82
// is iDrive's current audio source and built up. Without an iPhone, only Android Auto's route builds it; with one,
// iDrive's own answer is already yes. iDrive asks on its own thread, when it rebuilds the event. destroyApp puts the
// original vtable back.
const int kVtableHeader = 2, kCarPlaySlots = 22, kSlotAvailable = 11;
const int kSourceCarPlay = 0x82, kClassEntertainment = 1, kSinkAny = 0xff;

typedef bool (*AvailableFn)(const void* handler);
typedef int (*CurrentSourceFn)(const void* audio, int audioClass, int sink);
typedef bool (*BuiltUpFn)(const void* audio, int source);

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

void* symbolOf(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

// iDrive rebuilds the mode event (and asks the question above) when its players change, not when an audio source is
// built up: seen on the car on 22 November 2026, asked once as iDrive started and never again once Android Auto's
// route came up. What does run on iDrive's thread as that route comes up is the skip service making CarPlay's skip
// device (source 0x82) current, which calls its slot 0, getSkipDeviceClassName, for its log (and again on each skip).
// That skip device gets the same kind of copy: slot 0 answers as before and, when whether CarPlay's source plays has
// changed since the player was last asked, has the mode button service rebuild the event, which asks again.
// With the cluster on CarPlay's menu, its previous and next go through the skip service to this device, which asks
// CarPlay's database whether it plays (slot 4) and then skips it (slot 1): nothing, without an iPhone. So slot 4 also
// says yes while CarPlay's source plays, and slot 1 then hands the skip to the service ("skip 1" or "skip -1"),
// which sends the phone its next or previous track key. With an iPhone playing, both are iDrive's own.
const int kSkipSlots = 18, kSlotSkip = 1, kSlotPlaying = 4;
typedef void* (*ClassNameFn)(void* device);
typedef void (*SkipFn)(void* device, int direction);
typedef bool (*PlayingFn)(void* device);
typedef void (*NotifyFn)(void* modes);

void* g_carPlaySkip = nullptr;
void* g_carPlaySkipVptr = nullptr;
void* g_skipVtableCopy[kVtableHeader + kSkipSlots];
ClassNameFn g_className = nullptr;
SkipFn g_skip = nullptr;
PlayingFn g_playing = nullptr;
NotifyFn g_notify = nullptr;
bool g_notifying = false;

unsigned g_skipQuestions = 0, g_skipNames = 0;  // logged while few: where iDrive's own skip path stops

bool carPlaySkipPlaying(void* device) {
    const bool own = g_playing(device);
    const bool answer = own || (device == g_carPlaySkip && carPlaySourcePlaying());
    if (++g_skipQuestions <= 20) {
        alog("cluster player: iDrive asks whether CarPlay's skip device plays: %s", own      ? "yes, its own"
                                                                                    : answer ? "yes, Android Auto"
                                                                                             : "no");
    }
    return answer;
}

void carPlaySkip(void* device, int direction) {
    if (g_playing(device) || device != g_carPlaySkip || !carPlaySourcePlaying()) {
        g_skip(device, direction);
        return;
    }
    sendToService(direction > 0 ? "skip 1" : "skip -1");
    alog("cluster player: iDrive's cluster menu: %s track, sent to the service", direction > 0 ? "next" : "previous");
}

void* carPlaySkipClassName(void* device) {
    void* name = g_className(device);
    if (++g_skipNames <= 20) {
        alog("cluster player: the skip service uses CarPlay's skip device (made current, or a skip)");
    }
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
    // The skip device registered for source 0x82, from the skip service's map.
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
        alog("cluster player: no nudge (skip device for 0x%x %p, its class %s)", kSourceCarPlay, device,
             known ? "as expected" : "not as expected");
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

// On HmiMain3D's thread, after the probe found the mode button service.
void installClusterPlayer() {
    void** vtable = static_cast<void**>(symbolOf("_ZTV22CEntModeHandlerCarPlay"));
    void* availableFn = symbolOf("_ZNK19CEntModeHandlerBase22isModeHandlerAvailableEv");
    void** audioVtable = static_cast<void**>(symbolOf("_ZTV16CHmiAudioService"));
    g_audioGet = reinterpret_cast<GetCache>(
        symbolOf("_ZN9hmiplugin3getI16IHmiAudioServiceEENS_25CPluginInstanceCacheUsageIT_EEv"));
    g_currentSource = reinterpret_cast<CurrentSourceFn>(
        symbolOf("_ZNK16CHmiAudioService21getCurrentAudioSourceEN8HmiAudio11EAudioClassENS0_10EAudioSinkE"));
    g_builtUp = reinterpret_cast<BuiltUpFn>(symbolOf("_ZNK16CHmiAudioService20isAudioSourceBuiltUpE12EAudioSource"));
    // The audio service must be the class those two functions belong to.
    void* cache = g_audioGet ? g_audioGet() : nullptr;
    void* audio = cache ? *reinterpret_cast<void**>(static_cast<char*>(cache) + 8) : nullptr;
    g_audioVptr = audioVtable ? static_cast<void*>(audioVtable + kVtableHeader) : nullptr;
    const bool audioKnown =
        audio && g_audioVptr && *static_cast<void**>(audio) == g_audioVptr && g_currentSource && g_builtUp;
    // Find the CarPlay player among the registered ones.
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

bool entProbePeek(const void* addr, void* out, size_t n) {
    return peek(addr, out, n);
}

void entProbeClassName(const void* object, char* out, size_t size) {
    className(object, out, size);
}

bool clusterPlayerInstalled() {
    return g_carPlay != nullptr;
}

void releaseEntProbe() {
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
}

void captureEntProbe() {
    if (access(contract::kEntProbe, F_OK) != 0) {
        return;
    }
    if (pipe(g_pipe) != 0) {
        alog("ent probe: no pipe: not run");
        return;
    }
    g_modes = serviceFrom("_ZN9hmiplugin3getI21IEntModeButtonServiceEENS_25CPluginInstanceCacheUsageIT_EEv",
                          "_ZTV21CEntModeButtonService");
    g_skips = serviceFrom("_ZN9hmiplugin3getI15IEntSkipServiceEENS_25CPluginInstanceCacheUsageIT_EEv",
                          "_ZTV15CEntSkipService");
    logAll("as the app is created");
    g_lastDigest = walkHandlers(false) * 7 + walkSkips(false);
    if (access(contract::kClusterPlayer, F_OK) == 0) {
        installClusterPlayer();
    }
}

void entProbePoll(bool connected, long nowS) {
    if (!g_modes && !g_skips) {
        return;
    }
    if (g_carPlaySkip && connected) {
        // What iDrive was last told about CarPlay's player is what the cluster shows. Once a second: a service that
        // started again learns it too.
        sendToService(g_lastAnswer == 1 ? "clusterskip 1" : "clusterskip 0");
    }
    const uint32_t digest = walkHandlers(false) * 7 + walkSkips(false);
    if (digest != g_lastDigest) {
        logAll("changed");
    } else if (connected && nowS - g_lastLogS >= 60) {
        logAll("unchanged, a phone connected");
    } else {
        return;
    }
    g_lastDigest = digest;
    g_lastLogS = nowS;
}

}  // namespace app
}  // namespace evo
