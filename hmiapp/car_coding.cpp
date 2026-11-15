// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's coding from HmiMain3D's variant server (car_coding.hpp).
#include "car_coding.hpp"

#include <dlfcn.h>
#include <string.h>

#include <string>

#include "app_log.hpp"

namespace evo {
namespace app {

namespace {

const unsigned kSeriesValue = 0x958;  // BAUREIHE_VariantVal
const unsigned kFuelValue = 0x977;    // TYPE_OF_FUEL_VariantVal
const unsigned kLeftHand = 0x954;     // TYP_LENKUNG_linkslenker
const unsigned kRightHand = 0x955;    // TYP_LENKUNG_rechtslenker

enum { kSlotIsVariantSet = 3, kSlotGetVariantVal = 4, kSlotGetVehicleSpec = 14, kSlotGetProductionYear = 17 };

typedef void* (*GetCache)();
typedef bool (*IsVariantSet)(void* server, unsigned id);
typedef bool (*GetVariantVal)(void* server, unsigned id, unsigned char& value);
typedef void (*GetVehicleSpec)(void* server, std::string& first, std::string& second);
typedef int (*GetProductionYear)(void* server);
typedef const char* (*GetCodingName)(void* server, unsigned id);

void* g_server = nullptr;  // HmiMain3D's CHmiVariantServer, taken in createApp
GetCodingName g_codingName = nullptr;

void* symbol(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

void* slot(int i) {
    return (*static_cast<void***>(g_server))[i];
}

// The slot holds the function this was read from: another iDrive build may lay the vtable out differently.
bool slotIs(int i, const char* name) {
    void* f = symbol(name);
    return f && slot(i) == f;
}

bool named(unsigned id, const char* feature) {
    const char* name = g_codingName(g_server, id);
    return name && strcmp(name, feature) == 0;
}

}  // namespace

void captureVariantServer() {
    GetCache get = reinterpret_cast<GetCache>(
        symbol("_ZN9hmiplugin3getI17IHmiVariantServerEENS_25CPluginInstanceCacheUsageIT_EEv"));
    void* cache = get ? get() : nullptr;
    void* server = cache ? *reinterpret_cast<void**>(static_cast<char*>(cache) + 8) : nullptr;
    void** vtable = static_cast<void**>(symbol("_ZTV17CHmiVariantServer"));
    const bool known = server && vtable && *static_cast<void***>(server) == vtable + 2;
    g_codingName = reinterpret_cast<GetCodingName>(symbol("_ZN17CHmiVariantServer13getCodingNameE15VACodingFeature"));
    if (known && g_codingName) {
        g_server = server;
        const bool slots = slotIs(kSlotIsVariantSet, "_ZN17CHmiVariantServer12isVariantSetEj") &&
                           slotIs(kSlotGetVariantVal, "_ZN17CHmiVariantServer13getVariantValEjRh") &&
                           slotIs(kSlotGetVehicleSpec, "_ZN17CHmiVariantServer14getVehicleSpecER10THmiStringIcES2_") &&
                           slotIs(kSlotGetProductionYear, "_ZN17CHmiVariantServer17getProductionYearEv");
        if (!slots) {
            g_server = nullptr;
        }
        alog("car coding: variant server %p%s", server, slots ? "" : ", but its vtable is not as expected: not used");
        return;
    }
    alog("car coding: no variant server (get %p, cache %p, server %p, vtable %s, getCodingName %s)",
         reinterpret_cast<void*>(get), cache, server, known ? "as expected" : "not CHmiVariantServer's",
         g_codingName ? "found" : "missing");
}

bool readCarCoding(CarFacts& facts) {
    if (!g_server) {
        return false;
    }
    // isVACodingValid() always answers true; a feature has its name only once its coding is loaded.
    if (!g_codingName(g_server, kSeriesValue)) {
        alog("car coding: not loaded yet");
        return false;
    }
    const GetVariantVal value = reinterpret_cast<GetVariantVal>(slot(kSlotGetVariantVal));
    const IsVariantSet isSet = reinterpret_cast<IsVariantSet>(slot(kSlotIsVariantSet));

    unsigned char series = 0;
    const bool seriesRead = named(kSeriesValue, "BAUREIHE") && value(g_server, kSeriesValue, series);
    const char* code = seriesRead ? seriesOfCoding(series) : nullptr;
    facts.series = code ? code : "";

    // Logged only: the navigation's fuel, not the engine's (car_identity.hpp).
    unsigned char fuel = 0;
    const bool fuelRead = named(kFuelValue, "TYPE_OF_FUEL") && value(g_server, kFuelValue, fuel);

    const bool sideNamed = named(kLeftHand, "TYP_LENKUNG") && named(kRightHand, "TYP_LENKUNG");
    const bool left = sideNamed && isSet(g_server, kLeftHand);
    const bool right = sideNamed && isSet(g_server, kRightHand);
    facts.side = left != right ? (right ? "right" : "left") : "";

    std::string spec1, spec2;
    reinterpret_cast<GetVehicleSpec>(slot(kSlotGetVehicleSpec))(g_server, spec1, spec2);
    char shown1[64], shown2[64];
    maskForLog(spec1.c_str(), shown1, sizeof(shown1));
    maskForLog(spec2.c_str(), shown2, sizeof(shown2));
    const int year = reinterpret_cast<GetProductionYear>(slot(kSlotGetProductionYear))(g_server);

    alog(
        "car coding: series %s 0x%02x -> %s; steering %s left %d right %d -> %s; navigation fuel %s %u; production year"
        " %d; vehicle spec '%s' '%s'",
        seriesRead ? "read" : "NOT read", series, code ? code : "unknown", sideNamed ? "read" : "NOT read", left, right,
        facts.side.empty() ? "unknown" : facts.side.c_str(), fuelRead ? "read" : "NOT read", fuel, year, shown1,
        shown2);
    return true;
}

}  // namespace app
}  // namespace evo
