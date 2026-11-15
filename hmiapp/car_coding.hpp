// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's coding as iDrive holds it: its series, the driver's side and its fuel, read from HmiMain3D's variant server
// (libhmicoding.so's CHmiVariantServer). Reverse-engineered on the copied rootfs, 202306092158:
//   - hmiplugin::get<IHmiVariantServer>() (exported, weak, by libhmiaudio.so and others) returns, in r0, the calling
//     thread's plugin cache; the server is the pointer at +8. iDrive's own plugins call it and then the server's
//     virtual functions, with nothing to release afterwards (libhmispeedlock.so). The cache is per thread
//     (getCurrentThreadPluginScope), so the server is taken in createApp, on HmiMain3D's own thread (bt_audio.hpp).
//   - The server is a CHmiVariantServer: its vtable pointer is _ZTV17CHmiVariantServer + 8. Slots used: 3
//     isVariantSet(unsigned), 4 getVariantVal(unsigned, unsigned char&), 14 getVehicleSpec(THmiString<char>&,
//     THmiString<char>&) (two std::string copied from +0x34a4 and +0x34a8), 17 getProductionYear() (0, or 12 to 17
//     for 2012 to 2017 only). Slot 10, isVACodingValid(), always returns true. getCodingName(VACodingFeature) is not
//     virtual: it returns the coding's feature name ("BAUREIHE") once the coding is loaded, else null.
//   - Ids: each coding value is a row of libhmicoding.so's table (feature name, block, byte, value << 8 | mask, id, its
//     own name). BAUREIHE_VariantVal 0x958 (the series byte, car_identity.hpp), TYPE_OF_FUEL_VariantVal 0x977 (the fuel
//     byte), TYP_LENKUNG_linkslenker 0x954 and _rechtslenker 0x955 (left- and right-hand drive). The ids belong to this
//     firmware's coding table, so each is checked by its feature name before its value is used.
#ifndef AA_ENTRYNAV_HMIAPP_CAR_CODING_HPP
#define AA_ENTRYNAV_HMIAPP_CAR_CODING_HPP

#include "car_identity.hpp"

namespace evo {
namespace app {

// In createApp, on HmiMain3D's thread: finds the variant server and keeps it. Logs what it found.
void captureVariantServer();

// The series, driver's side and fuel into facts, from any thread once captured; each value only when its id carries
// the expected feature name. Logs the raw values. False while the coding isn't loaded yet (or there is no server).
bool readCarCoding(CarFacts& facts);

}  // namespace app
}  // namespace evo

#endif
