// Copyright (c) 2026 CallMeLewis
// iDrive's own source switch to Bluetooth audio (bt_audio.hpp).
#include "bt_audio.hpp"

#include <dlfcn.h>

#include "app_log.hpp"
#include "hmi_affinity.hpp"

namespace evo {
namespace app {

namespace {

const unsigned kBluetoothAudioFocusId = 2270;  // player type 13, "BLUE"
const int kSlotSendEvent = 5;                  // CHmiProcUnit: D1, D0, getID, getTimer, notify, sendEvent

typedef void* (*GetHmiProcUnit)();
typedef void* (*CreateEvent)(unsigned);
typedef void (*ActivatePlayer)(void* event, unsigned focusId);
typedef bool (*SendEvent)(void* queue, void* event);

void* g_queue = nullptr;  // HmiMain3D's IHmiProcUnit, taken in createApp

void* symbol(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

}  // namespace

void captureHmiQueue() {
    GetHmiProcUnit get = reinterpret_cast<GetHmiProcUnit>(symbol("_Z14getHmiProcUnitv"));
    g_queue = get && hmiThreadAllowed("getHmiProcUnit (Bluetooth audio)", kHmiMainThread) ? get() : nullptr;
    alog("bluetooth audio: HMI event queue %p%s", g_queue, get ? "" : " (getHmiProcUnit not found)");
}

bool sendBluetoothAudioSelect(const char** why) {
    if (!g_queue) {
        *why = "no HMI event queue: getHmiProcUnit gave none when the app was created";
        return false;
    }
    CreateEvent create = reinterpret_cast<CreateEvent>(symbol("_ZN25CHmiModePlayerSelectEvent11createEventEj"));
    ActivatePlayer activate =
        reinterpret_cast<ActivatePlayer>(symbol("_ZN25CHmiModePlayerSelectEvent14activatePlayerEj"));
    void* sendEvent = symbol("_ZN12CHmiProcUnit9sendEventEP9CHmiEvent");
    if (!create || !activate || !sendEvent) {
        *why = "iDrive's mode player select event or CHmiProcUnit::sendEvent not found";
        return false;
    }
    // The slot must be the sendEvent this was read from: another iDrive build may lay the vtable out differently.
    void** vtable = *static_cast<void***>(g_queue);
    if (vtable[kSlotSendEvent] != sendEvent) {
        *why = "the HMI event queue's slot 5 is not CHmiProcUnit::sendEvent on this iDrive";
        return false;
    }
    void* event = create(0);
    if (!event) {
        *why = "iDrive could not create the select event";
        return false;
    }
    activate(event, kBluetoothAudioFocusId);
    // The queue owns the event from here, as with iDrive's own source popup.
    reinterpret_cast<SendEvent>(vtable[kSlotSendEvent])(g_queue, event);
    *why = "sent";
    return true;
}

}  // namespace app
}  // namespace evo
