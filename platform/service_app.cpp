// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The aa-entrynav service's state and callbacks (see service_app.hpp).
#include "service_app.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "bluez_adapter.hpp"
#include "evo_log.hpp"
#include "evo_spp_snapshot.hpp"
#include "evo_touch.hpp"
#include "evo_trial_wireless.hpp"
#include "service_contract.hpp"
#include "tmp_file.hpp"
#include "usb_link.hpp"

namespace evo {

ServiceApp::ServiceApp(uint16_t p, bool v, volatile sig_atomic_t* quit)
    : knob(nullptr),
      server(p),
      shownWidth(1280),
      shownHeight(480),
      port(p),
      videoEnabled(v),
      stopping(false),
      wirelessDone(false),
      focus(true),
      ready(false),
      tcpPreparing(false),
      reverse(false),
      awaitShow(false),
      asking(false),
      inCall(false),
      pendingFocus(0),
      sppLinks(0),
      tcpLinks(0),
      videoFrames(0),
      wppReply(false),
      wppAccepted(false),
      usbStop(false),
      usbBusy(false),
      usbPhone(false),
      quit_(quit) {
    pthread_mutex_init(&mu, nullptr);
    pthread_mutex_init(&connectMu, nullptr);
    pthread_mutex_init(&videoMu, nullptr);
}

ServiceApp::~ServiceApp() {
    pill.stop();
    server.stop();
    mic.stop();
    kombi.stop();
    session.stop();
    audio.stop();  // after the session, whose thread hands it the phone's sound
    video.stop();
    bt.stop();
    pthread_mutex_destroy(&mu);
    pthread_mutex_destroy(&connectMu);
    pthread_mutex_destroy(&videoMu);
}

void ServiceApp::markReady() {
    pthread_mutex_lock(&mu);
    ready = true;
    pthread_mutex_unlock(&mu);
}

void ServiceApp::requestStop() {
    pthread_mutex_lock(&mu);
    stopping = true;
    pthread_mutex_unlock(&mu);
    usbStop = true;
}

bool ServiceApp::stopRequested() {
    pthread_mutex_lock(&mu);
    bool v = stopping;
    pthread_mutex_unlock(&mu);
    return v;
}

bool ServiceApp::projectionBusy() {
    pthread_mutex_lock(&mu);
    bool v = tcpPreparing;
    pthread_mutex_unlock(&mu);
    return v || session.isActive();
}

bool ServiceApp::wifiSessionRunning() {
    pthread_mutex_lock(&mu);
    const bool preparing = tcpPreparing;
    pthread_mutex_unlock(&mu);
    return !preparing && !usbBusy && session.isActive();
}

void ServiceApp::sppLinkOpened() {
    pthread_mutex_lock(&mu);
    ++sppLinks;
    pthread_mutex_unlock(&mu);
}

void ServiceApp::handshakeEnded(bool spoke, bool accepted) {
    pthread_mutex_lock(&mu);
    wppReply = wppReply || spoke;
    wppAccepted = wppAccepted || accepted;
    pthread_mutex_unlock(&mu);
}

ServiceApp::Turn ServiceApp::turn(bool service) {
    Turn t;
    pthread_mutex_lock(&mu);
    t.failed = wirelessDone;
    t.active = session.isActive();
    const uint32_t nowMs = monoMs();
    // A phone aside that the iDrive app never shows (app gone, its "show" lost): shown without it, and later phones no
    // longer wait for an app that may not be there. The app says "await" again when it starts.
    const bool gaveUp = t.active && asking && !focus && aside.expired(nowMs, reverse || inCall);
    if (gaveUp) {
        focus = true;
        asking = false;
        awaitShow = false;
        pendingFocus = 1;
    }
    t.onScreen = videoEnabled && t.active && focus && !reverse;  // what the iDrive app is told
    t.asks = t.active && asking && !focus;
    const PeekTiming::Picture picture = peek.turn(nowMs, t.onScreen);
    t.shown = picture.shown;
    if (!t.shown) {
        pill.hide();  // camera, menu, popup: the pill never stays over iDrive
    }
    t.focusMsg = pendingFocus;
    pendingFocus = 0;
    t.idleForAdvert = !tcpPreparing && !t.active && (service || (!sppLinks && !tcpLinks));
    pthread_mutex_unlock(&mu);
    if (gaveUp) {
        ELOG("ServiceApp",
             "no \"show\" from the iDrive app after %u s aside: Android Auto comes up by itself, and "
             "phones no longer wait aside until the app says \"await\" again",
             kAsideWaitMs / 1000);
    }
    t.pictureUp = false;
    t.pictureStarted = false;
    if (videoEnabled) {
        pthread_mutex_lock(&videoMu);
        video.setVisible(t.shown);
        const bool opacityRefused = t.shown && !video.setOpacity(picture.opacity) && picture.opacity < 1.f;
        t.pictureUp = video.isVisible();
        t.pictureStarted = video.isReady();
        pthread_mutex_unlock(&videoMu);
        if (opacityRefused) {
            ELOG("ServiceApp", "the compositor refused the opacity: the volume now steps the picture aside");
            pthread_mutex_lock(&mu);
            peek.refuseOpacity(monoMs(), nowMs);
            pthread_mutex_unlock(&mu);
        }
    }
    return t;
}

bool ServiceApp::refreshAdvertisementIfIdle(bool service, SppSnapshotReader& reader, unsigned slot) {
    // Same lock order as connected(): a TCP session cannot start during an idle refresh.
    bool failed = false;
    pthread_mutex_lock(&connectMu);
    pthread_mutex_lock(&mu);
    bool idle = !tcpPreparing && !session.isActive() && (service || (!sppLinks && !tcpLinks));
    pthread_mutex_unlock(&mu);
    if (idle) {
        failed = !refreshTrialAdvertisement(bt, reader, slot);
    }
    pthread_mutex_unlock(&connectMu);
    return !failed;
}

void ServiceApp::frame(uint32_t, const uint8_t* data, size_t len, uint64_t pts, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    if (a->videoEnabled) {
        // Phone frames never wait for mu: the main loop holds it while LayerManagerControl runs (show/hide,
        // opacity), which would stall the picture.
        ++a->videoFrames;
        a->video.postVideoFrame(data, len, pts);
        if (a->video.takeStall()) {
            // The picture would stay frozen for the rest of the session: ended as if the phone had left, so that the
            // cable or the Wi-Fi connects again with a new decoder.
            ELOG("ServiceApp", "video decoder stuck: the session ends and starts again");
            a->session.end();
        } else if (a->video.takeKeyframeRequest(monoMs()) && pthread_mutex_trylock(&a->mu) == 0) {
            // A frame was lost and the next ones build on it: the phone is told again that it has the screen, which
            // makes it start its stream afresh from an IDR. Only while it has the screen; never waits for mu.
            const bool projected = a->focus;
            pthread_mutex_unlock(&a->mu);
            if (projected) {
                ELOG("Video", "a frame was dropped: a new IDR asked for (video focus sent again)");
                a->session.postVideoFocus(true);
            }
        }
    }
}

void ServiceApp::codec(const uint8_t* data, size_t len, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    if (a->videoEnabled) {
        a->video.postCodecConfig(data, len);
    }
}

void ServiceApp::touchEvent(const aa::TouchEventOut& ev, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    a->session.postTouchEvent(toCanvas(ev, a->shownWidth, a->shownHeight));
}

// Volume key (knob or steering wheel). With the pill working, the level message that follows shows it.
void ServiceApp::volumeEvent(unsigned steps, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    const bool start = a->peek.volumeKey(monoMs(), a->pill.ready());
    const bool refused = a->peek.dimRefused();
    pthread_mutex_unlock(&a->mu);
    if (start) {
        ELOG("ServiceApp", "volume (%u step(s)): %s %u ms", steps, refused ? "AA picture aside" : "AA picture dimmed",
             kVolumePeekMs);
    }
}

// Volume level from the car (vMOST thread): the pill over Android Auto, which stays opaque.
void ServiceApp::volumeLevel(const VolumeLevel& level, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    const bool first = !a->peek.levelSeen();
    const bool ready = a->pill.ready();
    if (a->peek.volumeLevel(ready)) {
        a->pill.show(level.value, level.min, level.max);  // only takes the pill's own short lock
    }
    pthread_mutex_unlock(&a->mu);
    if (first) {
        ELOG("ServiceApp", "volume level received from the car: pill %s", ready ? "on" : "unavailable");
    }
}

// Hard key iDrive acts on (MENU, MEDIA, NAV, MAP, COM; not forwarded to the phone): the picture steps aside at
// once, so the result does not depend on timing (observed on the car otherwise: iDrive sometimes came back,
// sometimes not). If iDrive stays on the blank Android Auto page the iDrive app opens its main menu. The Android
// Auto row brings the phone back.
void ServiceApp::hardKey(const char* name, void* user) {
    // OPTION opens the Android Auto menu of the iDrive app (Stop, start automatically): see the app's hold logic.
    // NAV and MEDIA always go to Android Auto (appKey); COM reaches here only during a call.
    static const char* const leave[] = {"MENU", "MAP", "COM", "OPTIONS"};
    bool match = false;
    for (unsigned i = 0; i < sizeof(leave) / sizeof(leave[0]); ++i) {
        if (!strcmp(name, leave[i])) {
            match = true;
        }
    }
    if (!match) {
        return;
    }
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    const bool was = a->peek.hardKey();
    if (was) {
        a->focus = false;
        a->pendingFocus = 2;
        // A BACK or left tilt just before wrote the app key file. Left in place, the iDrive app would take iDrive
        // leaving its blank state for this key as that BACK, and bring it back under Android Auto.
        unlink(contract::kAppKeyFile);
    }
    pthread_mutex_unlock(&a->mu);
    if (was) {
        ELOG("ServiceApp",
             "key %s: AA picture aside, iDrive takes the screen back (the Android Auto row brings it back)", name);
    }
}

void ServiceApp::knobEvent(const aa::CcsEvent& ev, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    if (ev.kind == 1) {
        a->session.postKeyEvent(ev.keycode, ev.down != 0);
    } else if (ev.kind == 2) {
        a->session.postRelativeEvent(ev.keycode, ev.delta);
    }
}

// NAV / MEDIA / COM / steering wheel phone opened an Android Auto app, or BACK / a left tilt went to Android Auto
// (evo_knob.cpp): Android Auto stays on screen. iDrive may also have switched screens underneath; the iDrive app sees
// this file and puts iDrive back on its blank state.
void ServiceApp::appKey(const char* name, void* user) {
    (void)user;
    const std::string tmp = std::string(contract::kAppKeyFile) + ".tmp";
    const int fd = replaceTmpFile(tmp.c_str(), 0644);  // never through a link someone left in /tmp (tmp_file.hpp)
    if (fd >= 0) {
        char line[64];
        const int n = snprintf(line, sizeof(line), "appkey %s\n", name);
        const bool written = n > 0 && write(fd, line, static_cast<size_t>(n)) == n;
        close(fd);
        if (written) {
            rename(tmp.c_str(), contract::kAppKeyFile);
        }
    }
    ELOG("ServiceApp", "key %s: sent to Android Auto, which stays on screen; iDrive brought back under it", name);
}

// Commands from the iDrive app (service mode): show, hide, await, drivemode, popup, gear, call, daynight, quit.
void ServiceApp::command(const ControlCommand& c, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    if (c.verb == "show") {
        a->focus = true;
        a->asking = false;
        a->pendingFocus = 1;
    } else if (c.verb == "hide") {
        a->focus = false;
        a->asking = false;
        a->pendingFocus = 2;
    } else if (c.verb == "await") {
        // iDrive performs no screen request of an app while one of its own windows is on top, and the knob acts on
        // whatever iDrive shows under Android Auto. So the app first brings iDrive to its blank page, then says
        // "show": until then a phone that connects is served, its picture kept aside.
        if (!a->awaitShow) {
            ELOG("ServiceApp", "a phone that connects now waits aside for the iDrive app's \"show\"");
        }
        a->awaitShow = true;
    } else if (c.verb == "drivemode") {
        // Sent by the iDrive app once iDrive's driving mode display covers Android Auto's state: never while iDrive's
        // setting to show driving mode changes is off.
        if (a->peek.driveMode(monoMs())) {
            ELOG("ServiceApp", "iDrive's driving mode display: AA picture aside %u ms", kDriveModeShowMs);
        }
    } else if (c.verb == "popup") {
        const PeekTiming::Popup popup = a->peek.popup(monoMs(), c.arg != 0);
        if (popup == PeekTiming::kPopupShown) {
            ELOG("ServiceApp", "iDrive popup: AA picture aside %u ms at most", kPopupShowMs);
        } else if (popup == PeekTiming::kPopupGone) {
            ELOG("ServiceApp", "iDrive popup closed: AA picture back");
        }
    } else if (c.verb == "gear") {
        const bool r = c.arg == 2;  // CDS driving.gear reverse (2 = reverse), logged by the app
        if (r != a->reverse) {
            ELOG("ServiceApp", "reverse gear %s: AA picture %s", r ? "engaged" : "left", r ? "hidden" : "back");
        }
        a->reverse = r;
    } else if (c.verb == "call") {  // iDrive app (CDS communication.callStatus): phone keys go to iDrive's call screen
        a->inCall = c.arg != 0;
        if (a->knob) {
            a->knob->setPhoneToIdrive(c.arg != 0);
        }
        ELOG("ServiceApp", "call %s: phone keys %s", c.arg ? "in progress" : "ended",
             c.arg ? "left to iDrive" : "to Android Auto");
    }
    pthread_mutex_unlock(&a->mu);
    if (c.verb == "daynight") {
        a->applyDayNight(c.arg, "iDrive app");
    }
    if (c.verb == "quit") {
        *a->quit_ = 1;
    }
}

void ServiceApp::applyDayNight(int v, const char* src) {
    if (v != 1 && v != 2) {
        return;
    }
    session.postNightMode(v == 2);
    ELOG("ServiceApp", "car day/night = %s (%s): Android Auto theme %s", v == 2 ? "night" : "day", src,
         v == 2 ? "dark" : "light");
}

void ServiceApp::videoFocus(bool projected, uint32_t reason, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    a->focus = projected;
    // The phone decided for itself: a session still aside is no longer waiting for the iDrive app. Left set, the
    // phone's exit icon would make it wait again, and the app would bring Android Auto straight back.
    a->asking = false;
    pthread_mutex_unlock(&a->mu);
    ELOG("ServiceApp", "video focus=%s reason=%u", projected ? "AA" : "BMW", reason);
}

// Phone opened/closed the car mic (Assistant). Session thread: non-blocking.
void ServiceApp::micRequest(bool open, void* user) {
    static_cast<ServiceApp*>(user)->mic.request(open);
    ELOG("ServiceApp", "phone: microphone %s", open ? "open" : "closed");
}

void ServiceApp::micData(const uint8_t* pcm, size_t len, void* user) {
    static_cast<ServiceApp*>(user)->session.postMicData(pcm, len);
}

// AA navigation status (ch 10): next maneuver for the cluster/HUD. Session thread: non-blocking.
void ServiceApp::navStatus(uint16_t msgId, const uint8_t* body, size_t len, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    evo::kombi::Arrows::onNavStatus(msgId, body, len, &a->kombi);
}

void ServiceApp::ended(void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    // No stop()/join here: this callback runs on the AA session thread.
    a->mic.request(false);
    a->audio.sessionEnded();
    a->kombi.onSessionEnded();
    pthread_mutex_lock(&a->mu);
    a->focus = false;
    a->asking = false;
    pthread_mutex_unlock(&a->mu);
    a->server.notifySessionEnded();
    ELOG("ServiceApp", "session ended");
}

void ServiceApp::connected(int fd, void* user) {
    startSession(fd, user, std::string());
}

namespace {
// One question to the car's Bluetooth stack, asked on a thread of its own: a private D-Bus connection registers with
// libdbus's default timeout of about 25 s, which the session thread must not wait out (no ping answered meanwhile).
// Shared by the asking thread and the worker; the last one to let go deletes it.
struct PairedQuestion {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    std::string adapter, phone;
    int paired;  // bluezPaired's answer once done
    bool done;
    int refs;
    PairedQuestion(const std::string& a, const std::string& p)
        : adapter(a), phone(p), paired(-1), done(false), refs(2) {
        pthread_mutex_init(&mu, nullptr);
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&cv, &attr);
        pthread_condattr_destroy(&attr);
    }
    ~PairedQuestion() {
        pthread_cond_destroy(&cv);
        pthread_mutex_destroy(&mu);
    }
    void release() {  // with mu held; unlocks it
        const bool last = --refs == 0;
        pthread_mutex_unlock(&mu);
        if (last) {
            delete this;
        }
    }
};

const char* pairedText(int paired) {
    return paired == 1   ? "paired with the car"
           : paired == 0 ? "not paired with the car"
                         : "the car's stack did not say";
}

void* askPaired(void* p) {
    PairedQuestion* q = static_cast<PairedQuestion*>(p);
    const int paired = bluezPaired(q->adapter, q->phone);
    pthread_mutex_lock(&q->mu);
    const bool late = q->refs == 1;  // the asking thread stopped waiting
    q->paired = paired;
    q->done = true;
    pthread_cond_signal(&q->cv);
    if (late) {
        ELOG("Bluetooth", "phone %s: %s (answered late)", q->phone.c_str(), pairedText(paired));
    }
    q->release();
    return nullptr;
}

const unsigned kPairedWaitMs = 1500;
}  // namespace

bool ServiceApp::bluetoothPaired(const std::string& phone, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    const std::string adapter = a->bluetoothAdapter;
    pthread_mutex_unlock(&a->mu);
    PairedQuestion* q = new PairedQuestion(adapter, phone);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const bool asking = pthread_create(&t, &attr, askPaired, q) == 0;
    pthread_attr_destroy(&attr);
    if (!asking) {
        delete q;  // only for the log: not worth asking on this thread
        ELOG("Bluetooth", "phone %s: not asked (no thread)", phone.c_str());
        return false;
    }
    timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += kPairedWaitMs / 1000;
    until.tv_nsec += static_cast<long>(kPairedWaitMs % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        ++until.tv_sec;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&q->mu);
    while (!q->done && pthread_cond_timedwait(&q->cv, &q->mu, &until) == 0) {
    }
    const bool done = q->done;
    const int paired = q->paired;
    q->release();
    if (done) {
        ELOG("Bluetooth", "phone %s: %s", phone.c_str(), pairedText(paired));
    } else {
        ELOG("Bluetooth", "phone %s: the car's stack did not answer within %u ms; the session carries on",
             phone.c_str(), kPairedWaitMs);
    }
    return done && paired == 1;
}

bool ServiceApp::startSession(int fd, void* user, const std::string& carBluetooth, bool cable) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->connectMu);
    if (a->stopRequested()) {
        close(fd);
        pthread_mutex_unlock(&a->connectMu);
        return false;
    }
    if (cable && a->session.isActive()) {
        // The cable thread saw nothing running, then spent seconds asking the phone: a phone on Wi-Fi connected
        // meanwhile. Replacing it here would stop its session without its ended() (microphone, cluster, focus).
        ELOG("ServiceApp", "a phone on Wi-Fi connected while the cable was being set up: the cable waits");
        close(fd);
        pthread_mutex_unlock(&a->connectMu);
        return false;
    }
    pthread_mutex_lock(&a->mu);
    bool ready = a->ready;
    pthread_mutex_unlock(&a->mu);
    if (!ready) {
        close(fd);
        a->server.notifySessionEnded();
        pthread_mutex_unlock(&a->connectMu);
        return false;
    }
    pthread_mutex_lock(&a->mu);
    ++a->tcpLinks;
    a->tcpPreparing = true;
    pthread_mutex_unlock(&a->mu);
    ELOG("ServiceApp", "phone TCP connection received; preparing the video and the AA session");
    a->session.stop();  // join callbacks before destroying/reinitializing video buffers
    bool videoOk = true;
    // 1080p over the cable only. Over the car's Wi-Fi a phone that is asked for 1080p alone sets the session up, never
    // starts the picture and hangs up within seconds, so a phone on Wi-Fi always gets 720p.
    EvoVideoConfig config(false);
    if (a->videoEnabled) {
        pthread_mutex_lock(&a->videoMu);
        if (cable) {
            config = a->videoConfig;
        } else {
            config.paceMs = kWifiFrameSpacingMs;  // frames that come in pairs over the Wi-Fi spaced out (FramePacer)
        }
        a->video.stop();
        videoOk = a->video.init(config) && a->video.start();
        if (!videoOk && config.hd()) {
            // The decoder or the buffers refused 1080p: 720p from now on, for this run of the service.
            ELOG("ServiceApp", "the 1920x1080 picture did not start: 1280x720 from now on");
            a->video.stop();
            a->videoConfig = EvoVideoConfig(false);
            config = a->videoConfig;  // only the cable asks for 1080p, and the cable needs no pacing
            videoOk = a->video.init(config) && a->video.start();
        }
        pthread_mutex_unlock(&a->videoMu);
        if (!cable) {
            ELOG("ServiceApp", "phone on Wi-Fi: 1280x720%s, frames that come close together spaced %u ms apart",
                 a->videoConfig.hd() ? " (1920x1080 is for the cable)" : "", kWifiFrameSpacingMs);
        }
    }
    // The phone is told the canvas the decoder took; touches follow it.
    const bool hd = a->videoEnabled && config.hd();
    a->shownWidth = hd ? config.shownWidth : config.displayWidth;
    a->shownHeight = hd ? config.shownHeight : config.displayHeight;
    a->session.setHdVideo(hd);
    pthread_mutex_lock(&a->mu);
    if (!videoOk) {
        close(fd);
        a->server.notifySessionEnded();
        a->tcpPreparing = false;
        pthread_mutex_unlock(&a->mu);
        pthread_mutex_unlock(&a->connectMu);
        return false;
    }
    a->focus = !a->awaitShow;
    a->asking = a->awaitShow;
    a->aside.begin(monoMs());
    const bool aside = a->asking;
    pthread_mutex_unlock(&a->mu);
    if (aside) {
        ELOG("ServiceApp", "Android Auto starts aside: the iDrive app shows it once iDrive is on its page");
    }
    a->session.setBluetooth(carBluetooth, carBluetooth.empty() ? nullptr : bluetoothPaired, a);
    bool started = a->session.start(fd);
    if (!started) {
        a->server.notifySessionEnded();  // start owns/cleans fd even on failure
    } else {
        a->server.markSessionActive();
        if (!a->session.isActive()) {
            a->server.notifySessionEnded();
        }
    }
    pthread_mutex_lock(&a->mu);
    a->tcpPreparing = false;
    pthread_mutex_unlock(&a->mu);
    pthread_mutex_unlock(&a->connectMu);
    return started;
}

namespace {
const unsigned kCableRetryAfterFailureMs = 10000;

void usbLog(const char* line, void*) {
    ELOG("Usb", "%s", line);
}
}  // namespace

void* ServiceApp::usb(void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    LinuxUsbBus bus;
    UsbLink link(bus, usbLog, nullptr);
    // BMW's media program kept away from phones that appear from now on (usb_link.hpp). Without the rule the link
    // still works, as before: it races that program and wins within a few rounds.
    std::string ruleError;
    if (installUsbUdevRule(kUsbUdevRuleDir, ruleError)) {
        ELOG("Usb", "phones' USB files root-only from their next plug-in (%s/%s)", kUsbUdevRuleDir, kUsbUdevRuleName);
    } else {
        ELOG("Usb", "the udev rule could not be written (%s): BMW's media program may ask a phone first",
             ruleError.c_str());
    }
    bool carried = false;  // the cable carries the session that runs
    while (!a->usbStop) {
        if (carried && !link.active()) {
            const UsbBridge::Stats stats = link.stats();
            ELOG("Usb", "cable session over: %llu bytes from the phone, %llu to it, ended with %d",
                 static_cast<unsigned long long>(stats.fromPhone), static_cast<unsigned long long>(stats.toPhone),
                 stats.error);
            link.close();
            carried = false;
        }
        // Only while nothing is connected or being prepared: a phone on Wi-Fi keeps its session.
        bool present = carried;
        if (!carried && !a->projectionBusy() && link.phoneWaiting(&present)) {
            a->usbPhone = true;
            a->usbBusy = true;  // the wireless side does not send this phone onto the Wi-Fi meanwhile
            const int socket = link.connect(a->usbStop);
            if (socket >= 0) {
                carried = true;
                // A phone on a cable may not know the car's Bluetooth: it is told the address, and pairs if needed.
                BluezAdapter adapter;
                if (readBluezAdapter(adapter)) {
                    ELOG("Bluetooth", "car %s (%s): %s", adapter.address.c_str(), adapter.path.c_str(),
                         adapter.pairable ? "takes pairings now"
                                          : "takes no pairing now; one the phone starts may need the iDrive's "
                                            "new device screen");
                    pthread_mutex_lock(&a->mu);
                    a->bluetoothAdapter = adapter.path;
                    pthread_mutex_unlock(&a->mu);
                } else {
                    ELOG("Bluetooth", "the car's Bluetooth stack did not answer: the phone is not told about it");
                }
                // Owns the socket from here, as with Wi-Fi. A session that could not start (the video above all) is
                // not retried at once: each try resets the phone and asks it again, every second or so for as long as
                // the failure lasts.
                if (!startSession(socket, a, adapter.address, true)) {
                    ELOG("Usb", "the session did not start: the cable is tried again in %u s",
                         kCableRetryAfterFailureMs / 1000);
                    for (unsigned waited = 0; waited < kCableRetryAfterFailureMs && !a->usbStop; waited += 50) {
                        usleep(50000);
                    }
                }
            }
        }
        a->usbBusy = carried;
        a->usbPhone = present || carried;
        // A quarter of a second between looks: a phone just plugged in is asked before it has waited long.
        for (int i = 0; i < 5 && !a->usbStop; ++i) {
            usleep(50000);
        }
    }
    link.close();
    a->usbBusy = false;
    a->usbPhone = false;
    removeUsbUdevRule(kUsbUdevRuleDir);
    return nullptr;
}

void* ServiceApp::wireless(void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    runWirelessLoop(a->bt, a->ap, a->port, *a);
    pthread_mutex_lock(&a->mu);
    a->wirelessDone = true;
    pthread_mutex_unlock(&a->mu);
    return nullptr;
}

}  // namespace evo
