// Copyright (c) 2026 CallMeLewis
// The phone's track on iDrive's CarPlay "current audio file info" (cluster_title.hpp).
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
#include "cover_art.hpp"
#include "ent_probe.hpp"
#include "hmi_thread.hpp"
#include "utf16.hpp"

namespace evo {
namespace app {

namespace {

const unsigned char kCarPlayInstance = 0x70;
const unsigned kFileInfo = 0x38c0;          // the CAuxIn_CurrentAudioFileInfo in the proxy
const unsigned kMask = 0x7a, kData = 0x8c;  // in it: the field mask, and the CPRAudioMediaData
const unsigned kDelegateA = 0x4c, kDelegateB = 0x6c;
const size_t kMaxUnits = 200;
const long kReapplyS = 5;

// The three texts, in the order of the track file's lines: where each sits in the CPRAudioMediaData, and its bit in
// the first byte of the field mask (readPRMediaData: bit 1 +0x2e0, bit 3 +0x2e4, bit 5 +0x2e8; CEntInfoUpdaterAux
// shows them as title, artist and album).
const int kFields = 3;
const unsigned kFieldOffset[kFields] = {0x2e0, 0x2e4, 0x2e8};
const uint8_t kFieldBit[kFields] = {0x02, 0x08, 0x20};

typedef void* (*GetProxy)(unsigned char instance);
typedef void* (*StringFromUnits)(void* self, const uint16_t* units, int count);
typedef void* (*AssignString)(void* self, const void* other);
typedef void (*DestroyString)(void* self);
typedef void (*SetValid)(void* proxy, bool valid);
typedef void (*Invoke)(void* delegate, void* proxy);

struct Track {
    uint16_t units[kFields][kMaxUnits];
    size_t count[kFields];
    char coverHash[16];  // the track file's fourth line: the album art's hash, empty without one
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
Track g_track;           // what to show, under g_lock
std::string g_lastFile;  // status poll only
long g_lastAskS = 0;
// Per HMI thread, as each has its own proxy (hmi_thread.hpp): a track of ours is in this thread's proxy.
__thread bool t_wrote = false;

void* symbol(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

// The track file's four lines (title, artist, album, album art hash) as one string; empty without a file (no session).
std::string readTrackFile() {
    FILE* f = fopen(contract::kTrackFile, "r");
    if (!f) {
        return std::string();
    }
    std::string all;
    char line[512];
    for (int i = 0; i < kFields + 1 && fgets(line, sizeof(line), f); ++i) {
        line[strcspn(line, "\r\n")] = 0;
        all += line;
        all += '\n';
    }
    fclose(f);
    return all;
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

}  // namespace

void clusterTitlePoll(long nowS) {
    if (!clusterPlayerInstalled()) {
        return;
    }
    const std::string file = readTrackFile();
    const bool changed = file != g_lastFile;
    if (!changed && nowS - g_lastAskS < kReapplyS) {
        return;
    }
    if (changed) {
        Track track;
        size_t start = 0;
        for (int i = 0; i < kFields; ++i) {
            const size_t end = start < file.size() ? file.find('\n', start) : std::string::npos;
            const std::string text = end == std::string::npos ? std::string() : file.substr(start, end - start);
            track.count[i] = utf8ToUtf16(text.data(), text.size(), track.units[i], kMaxUnits);
            start = end == std::string::npos ? file.size() : end + 1;
        }
        const size_t end = start < file.size() ? file.find('\n', start) : std::string::npos;
        const std::string hash = end == std::string::npos ? std::string() : file.substr(start, end - start);
        snprintf(track.coverHash, sizeof(track.coverHash), "%s",
                 hash.size() < sizeof(track.coverHash) ? hash.c_str() : "");
        pthread_mutex_lock(&g_lock);
        g_track = track;
        pthread_mutex_unlock(&g_lock);
        g_lastFile = file;
        if (file.empty()) {
            alog("track info: none (no track)");
        } else {
            std::string shown = file;
            for (size_t i = 0; i + 1 < shown.size(); ++i) {
                if (shown[i] == '\n') {
                    shown[i] = '|';
                }
            }
            shown.resize(shown.size() - 1);
            alog("track info: %s", shown.c_str());
        }
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
    static __thread bool reported = false;
    // This thread's proxy: getServiceProxy answers from the calling thread's service node.
    char* proxy = getProxy ? static_cast<char*>(getProxy(kCarPlayInstance)) : nullptr;
    char* info = proxy ? proxy + kFileInfo : nullptr;
    const bool known = info && vtable && *reinterpret_cast<void**>(info) == static_cast<void*>(vtable + 2) &&
                       fromUnits && assign && destroy && setValid && invoke;
    if (!known) {
        if (!reported) {
            alog("track info: CarPlay's AuxIn proxy not as expected on this thread (proxy %p)",
                 static_cast<void*>(proxy));
            reported = true;
        }
        return;
    }
    Track track;
    pthread_mutex_lock(&g_lock);
    track = g_track;
    pthread_mutex_unlock(&g_lock);
    const bool empty = track.count[0] == 0 && track.count[1] == 0 && track.count[2] == 0;
    bool same = true;
    for (int i = 0; i < kFields; ++i) {
        same = same && sameText(info + kData + kFieldOffset[i], track.units[i], track.count[i]);
    }
    // HmiMain3D's own thread also has the screens that show a cover: a new picture has them ask again.
    const bool coverChanged = !onHmiThread() && coverUpdate(track.coverHash);
    if (!coverChanged && ((empty && !t_wrote) || same)) {
        return;  // nothing of ours to clear, or the track is already there
    }
    uint8_t bits = 0;
    for (int i = 0; i < kFields; ++i) {
        uint32_t made = 0;  // a THmiString is one pointer
        fromUnits(&made, track.units[i], static_cast<int>(track.count[i]));
        assign(info + kData + kFieldOffset[i], &made);
        destroy(&made);
        bits |= kFieldBit[i];
    }
    info[kMask] = static_cast<char>(info[kMask] | bits);  // these fields are in this status
    setValid(info, true);
    invoke(info + kDelegateA, info);
    invoke(info + kDelegateB, info);
    t_wrote = !empty;
    alog("track info: %u/%u/%u character(s) put in CarPlay's current audio file info (%s)",
         static_cast<unsigned>(track.count[0]), static_cast<unsigned>(track.count[1]),
         static_cast<unsigned>(track.count[2]), onHmiThread() ? "cluster's thread" : "main HMI thread");
}

}  // namespace app
}  // namespace evo
