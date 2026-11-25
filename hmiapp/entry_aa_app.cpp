// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// libAAEntryNav.so: Android Auto as a native iDrive "PluginApp" on EntryNav2 / EntryEVO (Linux), the framework of
// BMW's SecurityMenu / ActiveSeating. This file is the app object: its creation by HmiMain3D, the entry button and the
// menu, the focus of its states handed to the hold machine, and the status poll that keeps them in step with the
// service.
//
// EntryEVO facts (reverse-engineered on the copied rootfs):
//   - CHmiGenMain::start adds getDataPath(0)+"/Plugins" (-ro=/opt/hmi/data/ro) and getDataPath(1)+"/Plugins"
//     (-rw=/var/local/data/HMI). The writable one is mounted noexec (PerspartitionMount.sh: /var/local noexec,nosuid,
//     nodev; confirmed on the car), so dlopen fails there. The installation therefore adds
//     /opt/hmi/data/ro/Plugins/AAEntryNav and /opt/aa on the system partition (new folders only);
//     settings and logs stay in /var/local/data/AA (data only, nothing executed from there).
//   - ManagedPluginApp::init: {pluginDir}/AAEntryNav -> libAAEntryNav.so; dlsym "pluginAppInfo"; create at +0x10,
//     destroy at +0x14.
//   - CSpecialAppsHelper::getPluginAppConnector has no coding gate on this HU.
//   - ST_Ent_Selection is native state 20 here too (CRHMIEntertainmentInfo::onEntryPointEvent compares with 0x14).
// The other parts of the app:
//   bmw_framework  stand-ins for BMW's framework: declarations, the app's Runnable, the PluginApp vtable hooks
//   app_log        app.log, maintenance profile only
//   app_guards     off switch, crash guard, start automatically and its boot-loop guard
//   service_link   the supervisor /opt/aa/evo-trial, control messages, the service's state, the stop sequence
//   car_signals    gear, phone calls, day/night and driving mode from the car's CDS
//   hold_machine   iDrive kept on the blank state while Android Auto is on screen (pure, tested on the PC)
//   bt_audio       iDrive's own source switch to Bluetooth audio as the phone connects (when, in bt_audio_plan)
//   flare_page     the Flare Drive row: PIN, then a QR code with the car's Wi-Fi for the Flare Drive phone app
// Uninstall: delete the plugin folder and reboot.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../platform/service_contract.hpp"
#include "app_guards.hpp"
#include "app_log.hpp"
#include "app_step.hpp"
#include "bmw_framework.hpp"
#include "bt_audio.hpp"
#include "bt_audio_plan.hpp"
#include "car_coding.hpp"
#include "car_signals.hpp"
#include "cluster_player.hpp"
#include "cluster_title.hpp"
#include "cover_art.hpp"
#include "flare_page.hpp"
#include "hmi_affinity.hpp"
#include "hmi_thread.hpp"
#include "hold_machine.hpp"
#include "log_profile.hpp"
#include "mono_clock.hpp"
#include "rhmi_ids.hpp"
#include "service_link.hpp"
#include "service_state.hpp"

namespace evo {
namespace app {

namespace {

const unsigned kStatusPollMs = 1000;
const off_t kServiceLogLimit = 1024 * 1024;  // RAM: /tmp is tmpfs on the head unit
const unsigned kSpinMs = 200;

bmw::PluginApp* g_app = nullptr;     // created by HmiMain3D, until destroyed
bmw::PluginApp* g_runApp = nullptr;  // initialized, until finalized: the posted tasks run while it is set
long g_initTime = 0;                 // monotonic seconds

// ---- the hold (hmiapp/hold_machine.hpp): iDrive's focus in, iDrive events and control messages out ------------------
class AppHoldIo : public HoldIo {
public:
    void triggerEvent(unsigned event) override {
        if (g_app) {
            g_app->triggerEvent(event);
        }
    }
    void ownPopup(bool open) override {
        if (g_app) {
            g_app->triggerEvent(kEvOwnPopup, bmw::Parcel(open));
        }
    }
    void send(const char* message) override { sendToService(message); }
    void log(const char* line) override { alog("%s", line); }
    ServiceState readService() override { return readServiceState(); }
    bool appKeyAge(long* ageS) override {
        struct stat st;
        if (stat(contract::kAppKeyFile, &st) != 0) {
            return false;
        }
        *ageS = static_cast<long>(time(nullptr) - st.st_mtime);  // both from the car's clock
        return true;
    }
    void removeAppKey() override { unlink(contract::kAppKeyFile); }
};
AppHoldIo g_holdIo;
HoldMachine g_hold(g_holdIo);
BtAudioPlan g_btAudio;

// select-bt-audio present, and no sound over Wi-Fi (which does not use Bluetooth).
bool bluetoothAudioWanted() {
    return access(contract::kSelectBtAudio, F_OK) == 0 && access(contract::kAudioWifi, F_OK) != 0;
}

// As the phone connects: iDrive's sound to Bluetooth audio. The hold machine is told first, so iDrive opening its
// Bluetooth audio screen under Android Auto is held, not taken for a hard key.
void switchToBluetoothAudio(const ServiceState& d, long now) {
    const bool session = d.pid > 0 && d.is("connected");
    const bool busy = callActive() || lastGear() == kReverseGear;
    switch (g_btAudio.poll(session, session && bluetoothAudioWanted(), busy, now)) {
        case BtAudioPlan::kSwitch: {
            g_hold.expectScreenChange(now);
            const char* why = "";
            const bool sent = sendBluetoothAudioSelect(&why);
            alog("bluetooth audio: %s", sent ? "switch to Bluetooth audio sent to iDrive" : why);
            break;
        }
        case BtAudioPlan::kGiveUp:
            alog("bluetooth audio: not switched, a call or reverse gear lasted %d s after the phone connected",
                 BtAudioPlan::kWaitS);
            break;
        case BtAudioPlan::kNothing:
            break;
    }
}

// iDrive's focus on the blank state, our menu or its Help page, handed to the hold machine.
bool onAaFocus(void*, const bmw::Parcel& p) {
    g_hold.aaFocus(parcelBool(p));
    return true;
}
bool onAaVisible(void*, const bmw::Parcel& p) {
    g_hold.aaVisible(parcelBool(p), monoSeconds());
    return true;
}
bool menuFocus(unsigned state, const bmw::Parcel& p) {
    const bool focused = parcelBool(p);
    g_hold.menuFocus(state, focused, monoSeconds(), monoMs());
    return true;
}
bool onMenuFocus(void*, const bmw::Parcel& p) {
    flareMenuFocus(parcelBool(p));
    return menuFocus(kMenuState, p);
}
bool onHelpFocus(void*, const bmw::Parcel& p) {
    return menuFocus(kHelpState, p);
}
bool onFlarePinFocus(void*, const bmw::Parcel& p) {
    return menuFocus(kFlarePinState, p);
}
bool onFlarePageFocus(void* self, const bmw::Parcel& p) {
    flarePageFocus(static_cast<bmw::PluginApp*>(self), parcelBool(p));
    return menuFocus(kFlarePageState, p);
}

// ---- connecting animation of the entry button -----------------------------------------------------------------------
AppRunnable g_spinRunnable;
bool g_spinWanted = false, g_spinRunning = false;
unsigned g_spinFrame = 0;

void spinRun(void*) {
    bmw::PluginApp* app = g_runApp;
    if (!app) {
        g_spinRunning = false;
        return;
    }
    if (!g_spinWanted) {
        bmw::ImageID icon(kImgEntryIcon);
        app->updateModel(kEntryImageModel, icon);
        g_spinRunning = false;
        return;
    }
    bmw::ImageID frame(kImgSpinFirst + (g_spinFrame++ % kSpinFrames));
    app->updateModel(kEntryImageModel, frame);
    postRunnable(app, g_spinRunnable, kSpinMs);
}

void setConnectingAnimation(bmw::PluginApp* app, bool on) {
    if (on == g_spinWanted) {
        return;
    }
    g_spinWanted = on;
    bmw::Parcel busy(on);
    app->setProperty(kStartButton, kPropWaiting, busy);
    if (on && !g_spinRunning) {
        makeRunnable(g_spinRunnable, &spinRun);
        g_spinRunning = true;
        postRunnable(app, g_spinRunnable, kSpinMs);
    }
}

// ---- the entry button and the menu ----------------------------------------------------------------------------------
bool g_autostartShown = false;
void showAutostart(bmw::PluginApp* app) {
    app->updateModel(kAutostartModel, autostartOn());
    g_autostartShown = true;
}

bool guardedByOverlay(const char* what) {
    ServiceState d = readServiceState();
    if (d.onScreen()) {
        alog("%s ignored: Android Auto is on screen (stray knob input)", what);
        return true;
    }
    return false;
}

// Android Auto connected and nothing against showing it (stop in progress, call, reverse gear).
bool readyToShow(const ServiceState& d) {
    return d.pid > 0 && !strcmp(d.state, "connected") && !stopInProgress() && !callActive() &&
           lastGear() != kReverseGear;
}

void startServiceOrShow() {
    ServiceState d = readServiceState();
    if (d.pid > 0) {
        alog("start: service running (pid %d, %s): asking it to come to the front", d.pid, d.state);
        g_hold.requestShow();  // the coming overlay is new: hold iDrive on state 13
    } else if (d.pid < 0) {
        alog("start: service starting (%s): waiting for it", d.state);
    } else {
        launchService();
    }
}

bool onStartRow(void*, const bmw::Parcel&) {
    alog("menu: Start (raAction %u)", kActStart);
    if (stopInProgress()) {
        alog("start ignored: a stop is in progress");
        return true;
    }
    forgetBootGuard();
    // Connected: the row reads "Open Android Auto". Same path as the direct entry button (entry-direct), validated on
    // the car.
    if (readyToShow(readServiceState())) {
        g_hold.showAndHold("Open row", monoSeconds());
        return true;
    }
    startServiceOrShow();
    return true;
}

bool onStopRow(void*, const bmw::Parcel&) {
    alog("menu: Stop (raAction %u)", kActStop);
    if (stopInProgress() || guardedByOverlay("stop")) {
        return true;
    }
    beginStop();
    return true;
}

// Entry button "Android Auto" (iDrive Media menu): the app's menu, so Stop never requires Android Auto on screen;
// its first row opens Android Auto. Android Auto starts if it is not running (it comes up by itself once connected).
// With the entry-direct file: as CarPlay's, connected -> Android Auto on screen at once.
bool onEntry(void* self, const bmw::Parcel&) {
    bmw::PluginApp* app = static_cast<bmw::PluginApp*>(self);
    ServiceState d = readServiceState();
    alog("entry: Android Auto selected (service %s, pid %d)", d.state, d.pid);
    if (access(contract::kEntryDirect, F_OK) == 0 && readyToShow(d)) {
        g_hold.showAndHold("entry button", monoSeconds());
        return true;
    }
    app->triggerEvent(kEvOpenMenu);
    if (d.pid == 0 && !stopInProgress()) {
        forgetBootGuard();
        startServiceOrShow();
    }
    return true;
}

bool onAutostartToggle(void*, const bmw::Parcel&) {
    if (guardedByOverlay("start automatically")) {
        if (g_runApp) {
            showAutostart(g_runApp);  // the checkbox flipped on screen: show the unchanged setting again
        }
        return true;
    }
    toggleAutostart();
    if (g_runApp) {
        showAutostart(g_runApp);
    }
    return true;
}

// ---- the status poll: service state -> entry text, menu rows, hold, stop --------------------------------------------
AppRunnable g_statusRunnable;
unsigned g_shownText = kTextDefault;
unsigned g_shownRow = kTextStart;

void statusRun(void*) {
    bmw::PluginApp* app = g_runApp;
    if (!app) {
        return;
    }
    appStep(kStepStatus, "service");
    if (!g_autostartShown) {
        showAutostart(app);
    }
    resetGuardsWhenStable(g_initTime);
    if (boundServiceLog(contract::kServiceLog, contract::kServiceLogOld, kServiceLogLimit)) {
        alog("service log past %ld KiB: moved to %s", static_cast<long>(kServiceLogLimit / 1024),
             contract::kServiceLogOld);
    }
    serviceStop();
    serviceAutostart(g_initTime);
    ServiceState d = readServiceState();
    appStep(kStepStatus, "entry and menu");
    const bool connecting = d.is("connecting"), connected = d.is("connected");
    const bool waiting = d.is("waiting");  // cable only, no phone plugged in: nothing to animate
    unsigned want = connecting ? kTextConnecting : waiting ? kTextWaiting : kTextDefault;
    if (want != g_shownText) {
        bmw::TextID text(want);
        app->updateModel(kEntryTextModel, text);
        alog("entry text -> %u (%s)", want, d.state);
        g_shownText = want;
    }
    setConnectingAnimation(app, connecting);
    // Menu: "Stop Android Auto" only while it runs; the call state is (re)sent when the phone connects.
    static int s_stopShown = -1;
    const int stopWanted = d.pid != 0 ? 1 : 0;
    if (stopWanted != s_stopShown) {
        bmw::Parcel visible(stopWanted != 0);
        app->setProperty(kStopButton, kPropVisible, visible);
        s_stopShown = stopWanted;
    }
    // A (re)started service knows neither: started in reverse, Android Auto would come up over the camera.
    g_hold.tellService(d);
    unsigned row = connected ? kTextRowOpen : connecting ? kTextRowConnecting : waiting ? kTextRowWaiting : kTextStart;
    if (row != g_shownRow) {
        bmw::TextID text(row);
        app->updateModel(kStartRowModel, text);
        alog("start row text -> %u (%s)", row, d.state);
        g_shownRow = row;
    }
    const long now = monoSeconds();  // one moment for both
    appStep(kStepStatus, "Bluetooth audio");
    switchToBluetoothAudio(d, now);
    appStep(kStepStatus, "Flare page");
    flarePagePoll(app);
    appStep(kStepStatus, "cluster player");
    clusterPlayerPoll(d.pid, connected);
    appStep(kStepStatus, "cluster title");
    clusterTitlePoll(now);
    appStep(kStepStatus, "hold");
    g_hold.poll(d, now);
    // The heartbeat, once the whole poll has run: a step that never returns stops it (platform/app_heartbeat.hpp).
    static unsigned s_beats = 0;
    if (d.pid) {
        char beat[24];
        snprintf(beat, sizeof(beat), "alive %u", ++s_beats & 0x7fffffffu);
        sendToService(beat);
    }
    appStep(kStepStatus, "done");
    postRunnable(app, g_statusRunnable, kStatusPollMs);
}

// ---- MEDIA with iDrive on screen while Android Auto's sound plays (platform/media_key_jump.hpp) --------------------
// The service writes the file as MEDIA is pressed; Android Auto comes forward as with the Open row, and the service
// sends MEDIA to the phone once it gets the "show". Checked more often than the status, so the key feels immediate.
const unsigned kMediaJumpPollMs = 200;
const long kMediaJumpMaxAgeS = 2;  // older: a press the app did not see in time (iDrive busy), not acted on
AppRunnable g_mediaJumpRunnable;

void mediaJumpRun(void*) {
    bmw::PluginApp* app = g_runApp;
    if (!app) {
        return;
    }
    struct stat st;
    if (stat(contract::kMediaJumpFile, &st) == 0) {
        unlink(contract::kMediaJumpFile);
        const long age = static_cast<long>(time(nullptr) - st.st_mtime);  // both from the car's clock
        const ServiceState d = readServiceState();
        if (age < 0 || age > kMediaJumpMaxAgeS) {
            alog("MEDIA: request %ld s old, not acted on", age);
        } else if (d.onScreen()) {
            alog("MEDIA: Android Auto already on screen");
        } else if (!readyToShow(d)) {
            alog("MEDIA: Android Auto not shown (service %s, call or reverse gear, or stopping)", d.state);
        } else {
            g_hold.showAndHold("MEDIA key", monoSeconds());
        }
    }
    postRunnable(app, g_mediaJumpRunnable, kMediaJumpPollMs);
}

// ---- the car's CDS properties, subscribed once the app runs ---------------------------------------------------------
AppRunnable g_subRunnable;
void subscribeRun(void*) {
    if (!g_runApp) {
        return;
    }
    subscribeCarSignals(g_runApp, g_hold);
}

// ---- the app object -------------------------------------------------------------------------------------------------
bool appOnInit(bmw::PluginApp* self) {
    alog("onInit: begin");
    addAction(self, kActStart, &onStartRow);
    addAction(self, kActStop, &onStopRow);
    addAction(self, kActEntry, &onEntry);
    addAction(self, kActAutostart, &onAutostartToggle);
    addStateHandler(self, kMenuState, kHandlerFocus, &onMenuFocus);
    addStateHandler(self, kHelpState, kHandlerFocus, &onHelpFocus);
    registerFlarePage(self);
    addStateHandler(self, kFlarePinState, kHandlerFocus, &onFlarePinFocus);
    addStateHandler(self, kFlarePageState, kHandlerFocus, &onFlarePageFocus);
    addStateHandler(self, kAaState, kHandlerFocus, &onAaFocus);
    addStateHandler(self, kAaState, kHandlerVisible, &onAaVisible);
    g_initTime = monoSeconds();
    bool ok = self->onInit();
    alog("onInit: handlers registered, base onInit -> %d, start automatically %s", static_cast<int>(ok),
         autostartOn() ? "on" : "off");
    g_runApp = self;
    makeRunnable(g_subRunnable, &subscribeRun);
    postRunnable(self, g_subRunnable, 1500);
    makeRunnable(g_statusRunnable, &statusRun);
    postRunnable(self, g_statusRunnable, 2000);
    unlink(contract::kMediaJumpFile);  // a press from before this start is not acted on
    makeRunnable(g_mediaJumpRunnable, &mediaJumpRun);
    postRunnable(self, g_mediaJumpRunnable, 2000);
    return ok;
}

void appOnError(bmw::PluginApp*, unsigned code, const bmw::BasicString<char>& text) {
    const char* s = text.raw();
    alog("onError %u: %s", code, s ? s : "(null)");
}

void appOnFinalize(bmw::PluginApp* self) {
    alog("onFinalize");
    g_runApp = nullptr;
    self->onFinalize();
}

// What the app has iDrive's HMI threads do (hmi_thread.hpp): the track title on the cluster and main menu, and on
// HmiMain3D's own thread the read of iDrive's entertainment source.
void hmiThreadJob() {
    const AppStepSlot slot = onHmiThread() ? kStepHmiOther : kStepHmiMain;
    appStep(slot, "cluster title");
    clusterTitleApply();
    appStep(slot, "entertainment source");
    clusterPlayerSourceCheck();
    appStep(slot, "done");
}

bmw::PluginApp* createApp(bmw::IDispatcher& dispatcher, const char* name, const bmw::Bundle& params,
                          const bmw::Bundle& config) {
    mkdir(contract::kDataDir, 0755);
    alog("create(name=%s) %s, HMI pid %d", name ? name : "(null)", ENTRY_AA_APP_VERSION, static_cast<int>(getpid()));
    if (appDisabled() || !crashGuardAllows()) {
        return nullptr;
    }
    if (g_app) {
        alog("create: already created");
        return nullptr;
    }
    // HmiMain3D's own thread: the only one where its event queue can be looked up (bt_audio.hpp).
    rememberHmiMainThread();  // hmi_affinity.hpp
    captureHmiQueue();
    captureVariantServer();  // the same: the plugin cache it comes from is per thread (car_coding.hpp)
    captureClusterPlayer();  // the same, and active with audio-wifi (cluster_player.hpp)
    if (clusterPlayerInstalled()) {
        captureHmiThread(&hmiThreadJob);  // the same: getHmiProcUnit answers there (hmi_thread.hpp)
        coverInstall();                   // the same: CarPlay's screens live there (cover_art.hpp)
    }
    const PluginAppHooks hooks = {&appOnInit, &appOnError, &appOnFinalize};
    void* base = nullptr;
    bmw::PluginApp* app = newPluginApp(dispatcher, name, params, config, hooks, &base);
    g_app = app;
    alog("create: app %p (PluginApp vtable %p)", static_cast<void*>(app), base);
    return app;
}

void destroyApp(void* app) {
    alog("destroy(%p)", app);
    coverRelease();
    releaseClusterPlayer();  // whatever happens next: the library may go, and CarPlay's player calls into it
    closeAppStep();
    if (!app) {
        return;
    }
    deletePluginApp(app);
    if (app == g_app) {
        g_app = nullptr;
    }
}

__attribute__((constructor)) void libraryLoaded() {
    mkdir(contract::kDataDir, 0755);
    Dl_info info;
    memset(&info, 0, sizeof(info));
    const char* self =
        (dladdr(reinterpret_cast<void*>(&libraryLoaded), &info) && info.dli_fname) ? info.dli_fname : "?";
    alog("library loaded into pid %d from %s, %s; supervisor %s %s", static_cast<int>(getpid()), self,
         ENTRY_AA_APP_VERSION, contract::kSupervisor,
         access(contract::kSupervisor, X_OK) == 0 ? "executable" : "MISSING or not executable");
    countLoad();  // before create(): a crash in between counts too
}

}  // namespace
}  // namespace app
}  // namespace evo

// What ManagedPluginApp::init reads with dlsym: create at +0x10, destroy at +0x14. The library's only exported symbol.
extern "C" {
struct EntryPluginAppInfo {
    uint16_t versionMajor;
    uint16_t versionMinor;
    const char* name;
    const char* vendor;
    uint32_t rhmiInterfaceVersion;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
__attribute__((visibility("default"))) EntryPluginAppInfo pluginAppInfo = {
    1, 0, "Android Auto", "AAEntryNav", 1, &evo::app::createApp, &evo::app::destroyApp};
}
