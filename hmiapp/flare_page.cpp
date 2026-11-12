// Copyright (c) 2026 CallMeLewis
// The Flare Drive row, PIN screen and QR code page (flare_page.hpp).
//
// The speller's OK (resultAction) sends the whole PIN as a string, in the action's parameter map (onPinOkMap); the HMI
// then copies it into the result model, which is emptied straight away so the PIN does not stay in iDrive's memory.
// The speller stays open after OK (seen in the car): the next screen is always asked for with an event, and a return
// to the menu just after an OK is not taken for the driver backing out.
#include "flare_page.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../platform/service_contract.hpp"
#include "app_log.hpp"
#include "car_signals.hpp"
#include "flare_link.hpp"
#include "mono_clock.hpp"
#include "pin_lock.hpp"
#include "qr_code.hpp"
#include "qr_png.hpp"
#include "rhmi_ids.hpp"

namespace evo {
namespace app {

namespace {

const int kQrSide = 360;              // pixels, as the page's image component says (AAEntryNav.xml)
const long kAfterResultMs = 2000;     // a menu focus this soon after an OK is the speller closing, not BACK
const size_t kApConfMax = 16 * 1024;  // ap.conf is a few hundred bytes

PinLock g_lock(PinRecord(), 1);  // loaded from the file each time the row is selected
bool g_unlocked = false;         // the right PIN on this visit of the page
bool g_pageFocused = false;
bool g_codeShown = false;
unsigned g_status = 0;  // the page's text
long g_lastResultMs = -1000000;

// A small file into buf (cap bytes with its terminating 0). False if it is missing, a link, or does not fit.
bool readSmallFile(const char* path, char* buf, size_t cap) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    size_t n = 0;
    bool fits = true;
    for (;;) {
        if (n == cap - 1) {
            char extra;
            fits = read(fd, &extra, 1) <= 0;  // full: anything more means the file is too big
            break;
        }
        const ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) {
            break;
        }
        n += static_cast<size_t>(r);
    }
    close(fd);
    buf[n] = '\0';
    return fits;
}

PinRecord loadPin() {
    PinRecord r;
    char text[kPinRecordText + 16];
    if (readSmallFile(contract::kFlarePin, text, sizeof(text))) {
        if (parsePinRecord(text, r)) {
            return r;
        }
        alog("flare: %s is damaged: a new PIN will be chosen", contract::kFlarePin);
    }
    return PinRecord();
}

void savePin(const PinRecord& r) {
    char text[kPinRecordText];
    char tmp[sizeof(contract::kFlarePin) + 8];
    snprintf(tmp, sizeof(tmp), "%s.new", contract::kFlarePin);
    if (!formatPinRecord(r, text, sizeof(text))) {
        return;
    }
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    const size_t len = strlen(text);
    bool ok = fd >= 0 && write(fd, text, len) == static_cast<ssize_t>(len);
    if (fd >= 0) {
        ok = fsync(fd) == 0 && ok;
        close(fd);
    }
    ok = ok && rename(tmp, contract::kFlarePin) == 0;
    if (!ok) {
        unlink(tmp);
        alog("flare: could not save %s", contract::kFlarePin);
    }
}

void randomSalt(uint8_t salt[PinRecord::kSaltSize]) {
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    const bool ok = fd >= 0 && read(fd, salt, PinRecord::kSaltSize) == static_cast<ssize_t>(PinRecord::kSaltSize);
    if (fd >= 0) {
        close(fd);
    }
    if (!ok) {  // never seen on the head unit: the salt then only differs between cars and moments
        alog("flare: /dev/urandom unreadable, salt from the clock");
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint8_t seed[Sha256::kSize];
        const long mix[3] = {ts.tv_sec, ts.tv_nsec, static_cast<long>(getpid())};
        Sha256::hash(mix, sizeof(mix), seed);
        memcpy(salt, seed, PinRecord::kSaltSize);
    }
}

// The head unit's IPv4 address on its hotspot. False while the hotspot is down.
bool hotspotAddress(char* out, size_t cap) {
    const int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        return false;
    }
    struct ifreq rq;
    memset(&rq, 0, sizeof(rq));
    strncpy(rq.ifr_name, "wlan0", IFNAMSIZ - 1);
    const bool ok = ioctl(s, SIOCGIFADDR, &rq) == 0;
    close(s);
    if (!ok) {
        return false;
    }
    const struct sockaddr_in* in = reinterpret_cast<const struct sockaddr_in*>(&rq.ifr_addr);
    return inet_ntop(AF_INET, &in->sin_addr, out, static_cast<socklen_t>(cap)) != nullptr;
}

// The QR code to the page's image. False (and why in the log) when the hotspot's details are not there.
bool pushCode(bmw::PluginApp* app) {
    char* conf = static_cast<char*>(malloc(kApConfMax));
    CarWifi wifi;
    const bool read = conf && readSmallFile(contract::kWlandApConf, conf, kApConfMax);
    const bool parsed = read && parseApConf(conf, wifi);
    free(conf);
    char host[INET_ADDRSTRLEN] = "";
    if (!parsed || !hotspotAddress(host, sizeof(host))) {
        alog("flare: hotspot details missing: %s", !read     ? "ap.conf unreadable"
                                                   : !parsed ? "ap.conf not WPA2 with a passphrase"
                                                             : "wlan0 has no address");
        return false;
    }
    char link[512];
    const size_t len = flareDriveLink(wifi.ssid, wifi.password, host, link, sizeof(link));
    memset(&wifi, 0, sizeof(wifi));
    static QrCode code;
    const bool encoded = len > 0 && code.encode(reinterpret_cast<const unsigned char*>(link), len);
    memset(link, 0, sizeof(link));
    const size_t size = qrPngSize(kQrSide);
    unsigned char* png = encoded ? static_cast<unsigned char*>(malloc(size)) : nullptr;
    const bool drawn = png && qrPng(code, kQrSide, png, size);
    if (drawn) {
        showImage(app, kQrImageModel, png, size);  // the Image copies the bytes
        alog("flare: QR code shown (version %d, %u bytes of PNG, host %s)", code.version(), static_cast<unsigned>(size),
             host);
    } else {
        alog("flare: QR code not drawn (link %u bytes, encoded %d)", static_cast<unsigned>(len),
             static_cast<int>(encoded));
    }
    if (png) {
        memset(png, 0, size);
        free(png);
    }
    return drawn;
}

void setStatus(bmw::PluginApp* app, unsigned text, bool codeVisible) {
    if (text != g_status) {
        bmw::TextID t(text);
        app->updateModel(kFlareStatusModel, t);
        g_status = text;
    }
    bmw::Parcel visible(codeVisible);
    app->setProperty(kQrImage, kPropVisible, visible);
    g_codeShown = codeVisible;
}

void openPage(bmw::PluginApp* app, unsigned text, bool codeVisible) {
    setStatus(app, text, codeVisible);
    app->triggerEvent(kEvOpenFlarePage);
}

void openPin(bmw::PluginApp* app, unsigned title) {
    app->updateModel(kPinResultModel, "");
    bmw::TextID t(title);
    app->updateModel(kPinTitleModel, t);
    app->triggerEvent(kEvOpenPin);
}

// The right PIN, or a new one saved: the code, unless the car moves or the hotspot is down.
void unlock(bmw::PluginApp* app) {
    g_unlocked = true;
    if (carMoving()) {
        openPage(app, kTextFlareDriving, false);
    } else if (pushCode(app)) {
        openPage(app, kTextFlareScan, true);
    } else {
        openPage(app, kTextFlareWifiOff, false);
    }
}

bool onFlareRow(void* self, const bmw::Parcel&) {
    bmw::PluginApp* app = static_cast<bmw::PluginApp*>(self);
    g_unlocked = false;
    if (carMoving()) {
        alog("flare: row selected while moving");
        openPage(app, kTextFlareDriving, false);
        return true;
    }
    const long now = monoSeconds();
    g_lock = PinLock(loadPin(), now);
    if (g_lock.mode() == PinLock::kCheck && g_lock.lockedFor(now) > 0) {
        alog("flare: row selected, locked for %ld s more", g_lock.lockedFor(now));
        openPage(app, kTextFlareLocked, false);
        return true;
    }
    alog("flare: row selected, %s", g_lock.mode() == PinLock::kChoose ? "no PIN yet: choosing one" : "asking the PIN");
    openPin(app, g_lock.mode() == PinLock::kChoose ? kTextPinChoose : kTextPinEnter);
    return true;
}

void onPin(bmw::PluginApp* app, char* pin);

// The speller's OK reaches both handlers, the Parcel's first, in one dispatch (PluginApp::handleMessage): the text is
// in the parameter map (key 8) with the manifest's asyncCallbacks, and only the action's id in the Parcel. Whichever
// carries the text is used, once.
long g_pinTakenMs = -1000000;
const long kSameOkMs = 500;

bool onPinOkParcel(void* self, const bmw::Parcel& p) {
    char pin[32];
    parcelText(p, pin, sizeof(pin));
    if (pin[0]) {
        alog("flare: PIN from the action's data");
        g_pinTakenMs = monoMs();
        onPin(static_cast<bmw::PluginApp*>(self), pin);
    }
    return true;
}

// Each keystroke (a digit, "del" or "delall"): only there so the HMI keeps the text the OK sends. Never logged.
bool onPinInput(void*, const bmw::Map<unsigned, bmw::Variant>&) {
    return true;
}

bool onPinOkMap(void* self, const bmw::Map<unsigned, bmw::Variant>& params) {
    char pin[32];
    unsigned type = 0;
    const bool text = mapText(params, kSpellerTextKey, pin, sizeof(pin), &type);
    if (monoMs() - g_pinTakenMs < kSameOkMs) {
        memset(pin, 0, sizeof(pin));
        return true;  // the same OK, already taken from the Parcel
    }
    if (!text) {
        alog("flare: the speller's OK carried no text (parameter %u: type 0x%x)", kSpellerTextKey, type);
    } else if (!pin[0]) {
        alog("flare: the speller's OK carried an empty text");
    }
    onPin(static_cast<bmw::PluginApp*>(self), pin);
    return true;
}

void onPin(bmw::PluginApp* app, char* pin) {
    app->updateModel(kPinResultModel, "");
    g_lastResultMs = monoMs();
    uint8_t salt[PinRecord::kSaltSize];
    randomSalt(salt);
    const size_t typed = strlen(pin);
    const PinLock::Result r = g_lock.enter(pin, monoSeconds(), salt);
    memset(pin, 0, typed);
    if (g_lock.takeChanged()) {
        savePin(g_lock.record());
    }
    switch (r) {  // never the PIN in the log: its length only
        case PinLock::kInvalid:
            alog("flare: PIN refused (%u characters, not 4 to 8 digits)", static_cast<unsigned>(typed));
            openPin(app, kTextPinInvalid);
            break;
        case PinLock::kAskConfirm:
            alog("flare: new PIN typed once");
            openPin(app, kTextPinAgain);
            break;
        case PinLock::kMismatch:
            alog("flare: the new PIN differed the second time");
            openPin(app, kTextPinMismatch);
            break;
        case PinLock::kWrong:
            alog("flare: wrong PIN, %u tries before the lock", g_lock.triesLeft());
            openPin(app, kTextPinWrong);
            break;
        case PinLock::kLocked:
            alog("flare: locked for %ld s", g_lock.lockedFor(monoSeconds()));
            openPage(app, kTextFlareLocked, false);
            break;
        case PinLock::kChosen:
            alog("flare: new PIN saved");
            unlock(app);
            break;
        case PinLock::kOpen:
            alog("flare: PIN accepted");
            unlock(app);
            break;
    }
}

}  // namespace

void registerFlarePage(bmw::PluginApp* app) {
    addAction(app, kActFlareRow, &onFlareRow);
    addAction(app, kActPinOk, &onPinOkParcel);
    addMapAction(app, kActPinOk, &onPinOkMap);
    addMapAction(app, kActPinInput, &onPinInput);
}

void flarePageFocus(bmw::PluginApp* app, bool focused) {
    g_pageFocused = focused;
    if (!focused && (g_unlocked || g_codeShown)) {
        alog("flare: page left, code dropped");
        g_unlocked = false;
        setStatus(app, kTextFlareScan, false);
    }
}

void flareMenuFocus(bool focused) {
    if (focused && monoMs() - g_lastResultMs > kAfterResultMs) {
        g_lock.cancel();
    }
}

void flarePagePoll(bmw::PluginApp* app) {
    if (!g_pageFocused || !g_unlocked) {
        return;
    }
    const bool moving = carMoving();
    if (moving && g_codeShown) {
        alog("flare: the car moves: code hidden");
        setStatus(app, kTextFlareDriving, false);
    } else if (!moving && g_status == kTextFlareDriving) {
        if (pushCode(app)) {
            setStatus(app, kTextFlareScan, true);
        } else {
            setStatus(app, kTextFlareWifiOff, false);
        }
    }
}

}  // namespace app
}  // namespace evo
