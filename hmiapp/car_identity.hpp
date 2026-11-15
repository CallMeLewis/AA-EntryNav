// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the car says about itself, as lines of the vehicle file (core/sdr_builder.cpp): its series and driver's side
// from iDrive's coding (car_coding.hpp), and an id made from its VIN. The iDrive app writes them to contract::kCarFile;
// Flare Drive offers them to the owner, who decides whether they go into the vehicle file. Nothing here writes that
// file.
//
// Pure: no file, no clock. tests/test_car_identity.cpp.
#ifndef AA_ENTRYNAV_HMIAPP_CAR_IDENTITY_HPP
#define AA_ENTRYNAV_HMIAPP_CAR_IDENTITY_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "sha256.hpp"

namespace evo {
namespace app {

// The series code of a BAUREIHE coding byte (libhmicoding.so, 202306092158: va_BAUREIHE_*, the high byte of each
// value), as core/sdr_builder.cpp's kCarSeries names it; null for 0 (Unbekannt, unknown) and any other byte.
inline const char* seriesOfCoding(unsigned value) {
    struct Entry {
        unsigned char value;
        const char* code;
    };
    static const Entry kSeries[] = {
        {0x01, "F01"},  {0x02, "F02"},  {0x03, "F03"}, {0x04, "F04"},  {0x06, "F06"},  {0x07, "F07"},  {0x0a, "F10"},
        {0x0b, "F11"},  {0x0c, "F12"},  {0x0d, "F13"}, {0x0f, "F15"},  {0x10, "RR04"}, {0x11, "F16"},  {0x12, "F18"},
        {0x14, "F20"},  {0x15, "F21"},  {0x16, "F22"}, {0x17, "F23"},  {0x19, "F25"},  {0x1a, "F26"},  {0x21, "G11"},
        {0x22, "G12"},  {0x23, "G05"},  {0x24, "G06"}, {0x25, "G07"},  {0x26, "G14"},  {0x27, "G15"},  {0x28, "G16"},
        {0x2a, "G20"},  {0x2b, "G21"},  {0x2c, "G22"}, {0x2d, "G23"},  {0x2e, "G24"},  {0x2f, "G26"},  {0x30, "G30"},
        {0x31, "G31"},  {0x32, "G38"},  {0x33, "G32"}, {0x34, "RR21"}, {0x35, "RR22"}, {0x3a, "G28"},  {0x3b, "G29"},
        {0x3c, "J29"},  {0x40, "F30"},  {0x41, "F31"}, {0x42, "F32"},  {0x43, "F33"},  {0x44, "F34"},  {0x45, "F35"},
        {0x46, "F36"},  {0x47, "F40"},  {0x48, "F39"}, {0x49, "F44"},  {0x52, "F52"},  {0x54, "F54"},  {0x55, "F55"},
        {0x56, "F56"},  {0x57, "F57"},  {0x5a, "F60"}, {0x60, "I01"},  {0x61, "I12"},  {0x62, "I15"},  {0x65, "G01"},
        {0x66, "G02"},  {0x67, "G08"},  {0x70, "F45"}, {0x71, "F46"},  {0x72, "F47"},  {0x73, "F48"},  {0x74, "F49"},
        {0x80, "F80"},  {0x81, "RR31"}, {0x82, "F82"}, {0x83, "F83"},  {0x84, "F88"},  {0x85, "F85"},  {0x86, "F86"},
        {0x87, "F87"},  {0x88, "F97"},  {0x89, "F98"}, {0x90, "RR05"}, {0x91, "RR11"}, {0x92, "RR12"}, {0x93, "RR24"},
        {0x94, "RR14"}, {0x95, "RR06"}, {0x96, "F90"}, {0x97, "F91"},  {0x98, "F92"},  {0x99, "RR25"},
    };
    for (size_t i = 0; i < sizeof(kSeries) / sizeof(kSeries[0]); ++i) {
        if (kSeries[i].value == value) {
            return kSeries[i].code;
        }
    }
    return nullptr;
}

// No fuel is read: TYPE_OF_FUEL is the fuel the navigation's filling station search looks for, not the engine's. The
// reference F20, a petrol car, has 3 there, alle_kraftstoffe (all fuels). The owner sets the fuel in Flare Drive.

// n characters a VIN can hold (ISO 3779): 0-9 and A-Z without I, O and Q.
inline bool vinCharacters(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) || c == 'I' || c == 'O' || c == 'Q') {
            return false;
        }
    }
    return true;
}

// The serial number in a CDS text such as vehicle.VIN's, whatever its layout: the last 7 characters of the first run
// of 0-9 A-Z that is a whole VIN, else the first run that is 7 such characters, which is what iDrive gives (seen on the
// reference F20, 15 November 2026: {"VIN":"<7 characters>"}). BMW's last 7 VIN characters are unique to each car.
// False, with out empty, when there is neither.
inline bool vinSerialInText(const char* text, char out[8]) {
    out[0] = 0;
    const char* shortRun = nullptr;
    for (const char* p = text; p && *p;) {
        size_t n = 0;
        while ((p[n] >= '0' && p[n] <= '9') || (p[n] >= 'A' && p[n] <= 'Z')) {
            ++n;
        }
        if (n == 17 && vinCharacters(p, 17)) {
            shortRun = p + 10;
            break;
        }
        if (n == 7 && !shortRun && vinCharacters(p, 7)) {
            shortRun = p;
        }
        p += n ? n : 1;
    }
    if (!shortRun) {
        return false;
    }
    memcpy(out, shortRun, 7);
    out[7] = 0;
    return true;
}

// The vehicle id the phone files its settings for this car under (validVehicleId's form): a hash of the VIN's serial
// number shaped as a version 5 UUID. The same car always gets the same id, whether iDrive gives the whole VIN or its
// last 7 characters, and the id doesn't give the VIN away.
inline void vehicleIdOfSerial(const char* serial, char out[37]) {
    static const char kSalt[] = "AA-EntryNav vehicle id ";
    Sha256 h;
    h.update(kSalt, sizeof(kSalt) - 1);
    h.update(serial, strlen(serial));
    uint8_t d[Sha256::kSize];
    h.finish(d);
    d[6] = static_cast<uint8_t>((d[6] & 0x0f) | 0x50);
    d[8] = static_cast<uint8_t>((d[8] & 0x3f) | 0x80);
    static const char kHex[] = "0123456789abcdef";
    size_t o = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[o++] = '-';
        }
        out[o++] = kHex[d[i] >> 4];
        out[o++] = kHex[d[i] & 15];
    }
    out[o] = 0;
}

// text for the log, with every run of 7 or more letters and digits cut to its first 3 and the rest as '*': a whole VIN
// keeps only its maker code, and its 7-character serial number shows as 3 characters and 4 stars. Words such as "type"
// and codes such as "F20" stay. For the texts whose layout is being learnt; the VIN itself is never logged.
inline void maskForLog(const char* text, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; text && text[i] && o + 1 < cap;) {
        size_t n = 0;
        while ((text[i + n] >= '0' && text[i + n] <= '9') || (text[i + n] >= 'A' && text[i + n] <= 'Z') ||
               (text[i + n] >= 'a' && text[i + n] <= 'z')) {
            ++n;
        }
        if (n == 0) {
            out[o++] = text[i++];
            continue;
        }
        for (size_t k = 0; k < n && o + 1 < cap; ++k) {
            out[o++] = n >= 7 && k >= 3 ? '*' : text[i + k];
        }
        i += n;
    }
    out[o] = 0;
}

// What was read from the car. Empty fields were not found.
struct CarFacts {
    std::string series;     // "F20"
    std::string side;       // "left" or "right"
    std::string vehicleId;  // from the VIN's serial number
};

// The contract::kCarFile text: one vehicle file line per field found, in the vehicle file's order. Empty when nothing
// was found: the file then still says the app read the car.
inline std::string carFileText(const CarFacts& f) {
    std::string text;
    if (!f.series.empty()) {
        text += "series=" + f.series + "\n";
    }
    if (!f.vehicleId.empty()) {
        text += "id=" + f.vehicleId + "\n";
    }
    if (!f.side.empty()) {
        text += "side=" + f.side + "\n";
    }
    return text;
}

}  // namespace app
}  // namespace evo

#endif
