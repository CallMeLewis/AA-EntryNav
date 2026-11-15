// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The CDS handlers: car gear, phone calls, day/night and driving mode, to the service and the hold (car_signals.hpp).
#include "car_signals.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/service_contract.hpp"
#include "../platform/tmp_file.hpp"
#include "app_log.hpp"
#include "car_coding.hpp"
#include "car_identity.hpp"
#include "cds_parse.hpp"
#include "mono_clock.hpp"
#include "service_link.hpp"

namespace evo {
namespace app {

namespace {

const char* const kGearProperty = "driving.gear";
const char* const kCallProps[2] = {"communication.callStatus0", "communication.callStatus1"};
// Car day/night and driving mode (CDS names present in this HU's libhmiremoting).
const char* const kDayNightProperty = "vehicle.dayNightChangeOver";
const char* const kDriveModeProps[2] = {"driving.mode", "driving.mode35up"};

HoldMachine* g_hold = nullptr;

// ---- car gear -> service (reverse: the camera shows, Android Auto steps aside) --------------------------------------
int g_lastGear = kGearUnknown;
bool onGearChanged(void*, const bmw::BasicString<unsigned short>& value) {
    char text[160];
    narrow(value, text, sizeof(text));
    int gear = -1;
    for (size_t i = 0; text[i]; ++i) {
        if (text[i] >= '0' && text[i] <= '9') {
            gear = atoi(text + i);
            break;
        }
    }
    if (gear != g_lastGear) {
        alog("CDS %s = '%s' -> gear %d", kGearProperty, text, gear);
        g_lastGear = gear;
        g_hold->gearChanged(gear);
        char msg[32];
        snprintf(msg, sizeof(msg), "gear %d", gear);
        sendToService(msg);
    }
    return true;
}

// ---- phone calls: iDrive may show its own call screen; Android Auto shows the call too ------------------------------
bool g_callLine[2] = {false, false};
bool onCallStatus(int line, const bmw::BasicString<unsigned short>& value) {
    char text[200];
    narrow(value, text, sizeof(text));
    bool active = aa::cdsCallActive(text);
    bool was = callActive();
    g_callLine[line] = active;
    g_hold->callChanged(callActive(), monoSeconds());
    alog("CDS %s = '%s' -> %s", kCallProps[line], text, active ? "in a call" : "idle");
    if (was != callActive()) {
        sendToService(callActive() ? "call 1" : "call 0");  // phone keys follow iDrive's call
    }
    return true;
}
bool onCallStatus0(void*, const bmw::BasicString<unsigned short>& v) {
    return onCallStatus(0, v);
}
bool onCallStatus1(void*, const bmw::BasicString<unsigned short>& v) {
    return onCallStatus(1, v);
}

// ---- day and night --------------------------------------------------------------------------------------------------
int g_lastDayNight = -1000;
bool onDayNightChanged(void*, const bmw::BasicString<unsigned short>& value) {
    char text[160];
    narrow(value, text, sizeof(text));
    int v = aa::cdsIntField(text, "dayNightChangeOver", -1);
    if (v == g_lastDayNight) {
        return true;
    }
    g_lastDayNight = v;
    alog("CDS %s = '%s' -> %d (1 day, 2 night, 0 no change, 3 invalid)", kDayNightProperty, text, v);
    if (v != 1 && v != 2) {
        return true;
    }
    // Replaced whole, never through a link someone left in /tmp (tmp_file.hpp).
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", contract::kDayNightFile);
    const int fd = replaceTmpFile(tmp, 0644);
    if (fd >= 0) {
        char line[32];
        const int n = snprintf(line, sizeof(line), "daynight %d\n", v);
        const bool written = n > 0 && write(fd, line, static_cast<size_t>(n)) == n;
        close(fd);
        if (written) {
            rename(tmp, contract::kDayNightFile);
        }
    }
    char msg[32];
    snprintf(msg, sizeof(msg), "daynight %d", v);
    sendToService(msg);
    return true;
}

// ---- speed: only whether the car moves, for the Flare Drive page ----------------------------------------------------
const char* const kSpeedProperty = "driving.speedActual";
int g_moving = -1;  // -1 until the first speed read, then 0 or 1
bool g_speedSeen = false;
bool onSpeedChanged(void*, const bmw::BasicString<unsigned short>& value) {
    char text[160];
    narrow(value, text, sizeof(text));
    const int kmh = aa::cdsIntField(text, "speedActual", -1);
    const int moving = kmh < 0 ? g_moving : kmh >= kMovingKmh ? 1 : 0;
    if (moving != g_moving || !g_speedSeen) {  // the first value (its layout), then each start and stop
        alog("CDS %s = '%s' -> %s", kSpeedProperty, text, moving < 0 ? "unknown" : moving ? "moving" : "stopped");
    }
    g_speedSeen = true;
    g_moving = moving;
    return true;
}

// ---- driving mode ---------------------------------------------------------------------------------------------------
int g_lastDriveMode[2] = {-1000, -1000};
long g_driveModeSubAt = -1;
bool onDriveMode(int prop, const bmw::BasicString<unsigned short>& value) {
    char text[160];
    narrow(value, text, sizeof(text));
    int v = aa::cdsIntField(text, "mode", -1);
    int was = g_lastDriveMode[prop];
    if (v == was) {
        return true;
    }
    const int invalid = prop == 1 ? 63 : 15;  // "not valid" values
    alog("CDS %s = '%s' -> mode %d%s", kDriveModeProps[prop], text, v, v == invalid || v < 0 ? " (not valid)" : "");
    g_lastDriveMode[prop] = v;
    const bool valid = v >= 0 && v != invalid;
    // A change between two valid modes, or a first valid value well after subscribing (some variants send none).
    const bool change = valid && ((was >= 0 && was != invalid) ||
                                  (was < 0 && g_driveModeSubAt >= 0 && monoSeconds() - g_driveModeSubAt >= 5));
    if (!change) {
        return true;
    }
    // The service is told only if iDrive's driving mode display then covers Android Auto's state (popup_peek.hpp).
    alog("drive mode change to %d", v);
    g_hold->driveModeChanged(monoSeconds());
    return true;
}
bool onDriveMode0(void*, const bmw::BasicString<unsigned short>& v) {
    return onDriveMode(0, v);
}
bool onDriveMode1(void*, const bmw::BasicString<unsigned short>& v) {
    return onDriveMode(1, v);
}

// ---- what the car says about itself, for Flare Drive (car_identity.hpp) --------------------------------------------
CarFacts g_facts;
bmw::PluginApp* g_app = nullptr;
AppRunnable g_codingRunnable;
int g_codingTries = 0;
const int kCodingTries = 12;  // one a few seconds apart: the coding is normally loaded long before the app runs
const unsigned kCodingRetryMs = 5000;

void writeCarFile() {
    const std::string text = carFileText(g_facts);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", contract::kCarFile);
    const int fd = replaceTmpFile(tmp, 0644);
    if (fd < 0) {
        return;
    }
    const bool written = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    close(fd);
    if (written) {
        rename(tmp, contract::kCarFile);
    }
}

void readCodingRun(void*) {
    ++g_codingTries;
    if (readCarCoding(g_facts)) {
        writeCarFile();
    } else if (g_codingTries < kCodingTries && g_app) {
        postRunnable(g_app, g_codingRunnable, kCodingRetryMs);
    } else {
        alog("car coding: not read after %d tries", g_codingTries);
        writeCarFile();  // the VIN's id, if any: the file says the app has looked
    }
}

// The VIN, or any part of it, is never logged: only how long the text is and whether a serial number was found.
const char* const kVinProperty = "vehicle.VIN";
bool onVinChanged(void*, const bmw::BasicString<unsigned short>& value) {
    char text[160], serial[8];
    narrow(value, text, sizeof(text));
    const bool found = vinSerialInText(text, serial);
    char id[37] = "";
    if (found) {
        vehicleIdOfSerial(serial, id);
    }
    if (g_facts.vehicleId != id) {
        alog("CDS %s: %u characters -> %s", kVinProperty, static_cast<unsigned>(strlen(text)),
             found ? "vehicle id from its serial number" : "no serial number");
    }
    g_facts.vehicleId = id;
    writeCarFile();
    return true;
}

const char* const kVehicleTypeProperty = "vehicle.type";
bool onVehicleType(void*, const bmw::BasicString<unsigned short>& value) {
    char text[200], shown[200];
    narrow(value, text, sizeof(text));
    maskForLog(text, shown, sizeof(shown));
    alog("CDS %s = '%s'", kVehicleTypeProperty, shown);
    return true;
}

}  // namespace

void subscribeCarSignals(bmw::PluginApp* app, HoldMachine& hold) {
    g_hold = &hold;
    g_app = app;
    makeRunnable(g_codingRunnable, &readCodingRun);
    readCodingRun(nullptr);
    addCdsHandler(app, kVinProperty, &onVinChanged);
    addCdsHandler(app, kVehicleTypeProperty, &onVehicleType);
    alog("CDS handlers for %s and %s registered", kVinProperty, kVehicleTypeProperty);
    addCdsHandler(app, kGearProperty, &onGearChanged);
    alog("CDS handler for %s registered", kGearProperty);
    for (int i = 0; i < 2; ++i) {
        addCdsHandler(app, kCallProps[i], i == 0 ? &onCallStatus0 : &onCallStatus1);
        alog("CDS handler for %s registered", kCallProps[i]);
    }
    addCdsHandler(app, kDayNightProperty, &onDayNightChanged);
    alog("CDS handler for %s registered", kDayNightProperty);
    g_driveModeSubAt = monoSeconds();
    for (int i = 0; i < 2; ++i) {
        addCdsHandler(app, kDriveModeProps[i], i == 0 ? &onDriveMode0 : &onDriveMode1);
        alog("CDS handler for %s registered", kDriveModeProps[i]);
    }
    addCdsHandler(app, kSpeedProperty, &onSpeedChanged);
    alog("CDS handler for %s registered", kSpeedProperty);
}

bool carMoving() {
    return g_moving == 1;
}

bool callActive() {
    return g_callLine[0] || g_callLine[1];
}

int lastGear() {
    return g_lastGear;
}

}  // namespace app
}  // namespace evo
