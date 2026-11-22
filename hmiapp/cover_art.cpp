// Copyright (c) 2026 CallMeLewis
// The phone's album art as iDrive's CarPlay cover (cover_art.hpp).
#include "cover_art.hpp"

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>

#include "../platform/service_contract.hpp"
#include "app_log.hpp"

namespace evo {
namespace app {

namespace {

const int kQueryCoverArtSlot = 15;  // CEntInfoUpdaterCarPlay's vtable, after offset to top and typeinfo
const unsigned kUpdaterAdapter = 0x20;
const int kSetCoverSlot = 37;            // IEntInfoAdapter::setAndScaleDynamicCoverArtForMM(const SHmiResourceHandle&)
const int kSuppliers[2] = {0x41, 0x42};  // two bitmaps in turn: a new picture gets a handle the screen hasn't got
const size_t kStreamBytes = 64;          // a CSvcStream is 0x1c bytes (CHmiBufferStream's fields)

typedef void (*UpdaterFn)(void* updater);
typedef void (*SetCover)(void* adapter, const uint32_t* handle);
typedef void (*StreamCtor)(void* stream);
typedef void (*StreamDtor)(void* stream);
typedef bool (*ReadFile)(void* stream, const char* path);
typedef int (*Convert)(void* manager, void* context, int supplier, uint32_t* handle, void* stream, int* width,
                       int* height);

void** g_slot = nullptr;  // the slot in iDrive's vtable, while swapped
UpdaterFn g_original = nullptr;
// The app's CHmiBitmapManager: a std::map<SSupplierID, SHmiResourceHandle>, its comparator word, then the header
// {colour, parent, left, right} and the count. Empty: left and right point at the header.
uint32_t g_manager[6];
uint32_t g_handle[2];
int g_shown = -1;  // which of g_handle the screens get, -1: none (iDrive's own query)
std::string g_hash;

void* symbol(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

bool setSlot(void** slot, void* value) {
    const long page = sysconf(_SC_PAGESIZE);
    char* start = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(page - 1));
    if (mprotect(start, page, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    *slot = value;
    mprotect(start, page, PROT_READ);
    return true;
}

// In place of CEntInfoUpdaterAux::queryCoverArt for CarPlay's screens, on HmiMain3D's thread.
void queryCoverArt(void* updater) {
    void* adapter = g_shown >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(updater) + kUpdaterAdapter) : nullptr;
    if (!adapter) {
        g_original(updater);
        return;
    }
    void** vtable = *static_cast<void***>(adapter);
    reinterpret_cast<SetCover>(vtable[kSetCoverSlot])(adapter, &g_handle[g_shown]);
}

}  // namespace

void coverInstall() {
    void** vtable = static_cast<void**>(symbol("_ZTV22CEntInfoUpdaterCarPlay"));
    void* original = symbol("_ZN18CEntInfoUpdaterAux13queryCoverArtEv");
    const uint32_t* invalid = static_cast<const uint32_t*>(symbol("_ZN18SHmiResourceHandle21ResourceHandleInvalidE"));
    const bool helpers = symbol("_ZN10CSvcStreamC1Ev") && symbol("_ZN10CSvcStreamD1Ev") &&
                         symbol("_ZN16CHmiBufferStream12readFromFileEPKc") &&
                         symbol(
                             "_ZN17CHmiBitmapManager25convertCSvcStreamToBitmapER11IHmiContext17EBitmapSupplierIDR18"
                             "SHmiResourceHandleR10CSvcStreamRiS7_");
    void** slot = vtable ? vtable + 2 + kQueryCoverArtSlot : nullptr;
    if (!slot || !original || *slot != original || !invalid || !helpers) {
        alog("cover art: not available (updater vtable %p, slot %s, helpers %s)", static_cast<void*>(vtable),
             slot && original && *slot == original ? "as expected" : "not as expected", helpers ? "found" : "missing");
        return;
    }
    memset(g_manager, 0, sizeof(g_manager));
    g_manager[3] = g_manager[4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_manager[1]));
    g_handle[0] = g_handle[1] = *invalid;
    g_original = reinterpret_cast<UpdaterFn>(original);
    if (!setSlot(slot, reinterpret_cast<void*>(&queryCoverArt))) {
        alog("cover art: iDrive's vtable could not be made writable");
        return;
    }
    g_slot = slot;
    alog("cover art: CarPlay's screens ask the app for their cover");
}

bool coverUpdate(const char* hash) {
    if (!g_slot || g_hash == hash) {
        return false;
    }
    g_hash = hash;
    if (!*hash) {
        g_shown = -1;
        alog("cover art: none");
        return true;
    }
    static StreamCtor construct = reinterpret_cast<StreamCtor>(symbol("_ZN10CSvcStreamC1Ev"));
    static StreamDtor destruct = reinterpret_cast<StreamDtor>(symbol("_ZN10CSvcStreamD1Ev"));
    static ReadFile readFile = reinterpret_cast<ReadFile>(symbol("_ZN16CHmiBufferStream12readFromFileEPKc"));
    static Convert convert = reinterpret_cast<Convert>(
        symbol("_ZN17CHmiBitmapManager25convertCSvcStreamToBitmapER11IHmiContext17EBitmapSupplierIDR18"
               "SHmiResourceHandleR10CSvcStreamRiS7_"));
    const int slot = g_shown == 0 ? 1 : 0;
    uint32_t stream[kStreamBytes / 4];
    construct(stream);
    const bool read = readFile(stream, contract::kCoverFile);
    int width = -1, height = -1, result = -1;
    if (read) {
        // Its IHmiContext is not used (radio's CRadioPictureLoader passes its own).
        result = convert(g_manager, nullptr, kSuppliers[slot], &g_handle[slot], stream, &width, &height);
    }
    destruct(stream);
    if (!read || width <= 0 || height <= 0) {
        alog("cover art: %s not made into a bitmap (%s, result %d)", hash, read ? "read" : "not read", result);
        g_shown = -1;
        return true;
    }
    g_shown = slot;
    alog("cover art: %s, %dx%d, bitmap 0x%08x", hash, width, height, g_handle[slot]);
    return true;
}

void coverRelease() {
    if (g_slot && setSlot(g_slot, reinterpret_cast<void*>(g_original))) {
        g_slot = nullptr;
    }
}

}  // namespace app
}  // namespace evo
