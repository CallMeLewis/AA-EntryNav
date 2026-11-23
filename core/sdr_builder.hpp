// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The ServiceDiscoveryResponse that describes this car to the phone, built from Google's DHU template.
#ifndef AA_ENTRYNAV_CORE_SDR_BUILDER_HPP
#define AA_ENTRYNAV_CORE_SDR_BUILDER_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "canvas.hpp"

namespace aa {

// The fuel the car takes, as Android Auto's FuelType: Maps uses it for fuel stops.
enum CarFuel { kFuelPetrol = 1, kFuelDiesel = 4 };  // UNLEADED, DIESEL_2 (road diesel)

// What the phone is told about the car and the head unit.
struct SdrIdentity {
    std::string carMake, carModel, carYear, vehicleId;
    std::string huMake, huModel, swBuild, swVersion;
    bool rightHandDrive;
    CarFuel fuel;
    SdrIdentity();  // this car and head unit: left-hand drive, petrol
    // Model, year and id of a car other than the reference one, written at installation ("model=320d xDrive",
    // "year=2021", "id=<uuid>", one per line), and its driver's side and fuel ("side=right", "fuel=diesel"). A missing
    // file or an invalid line keeps the reference values. False if unreadable. Without an id= line the reference id
    // stays, so a phone keeps the settings it already has.
    // A "series=F20" line from findCarSeries sets the make and adds the code to the model: "M140i (F20)" from
    // model=M140i, "1 Series (F20)" without a model= line. A model that already has a bracket keeps it as written.
    bool loadVehicleFile(const char* path);
};
// Accepted model and year: 1 to 32 of A-Z a-z 0-9 space . _ - ( ), and 1xxx or 2xxx.
bool validCarModel(const std::string& s);

// A series code as iDrive's coding names it (BAUREIHE), with the make and the series the phone shows. name is null for
// a code whose cars we can't name: the phone then shows the code itself.
struct CarSeries {
    const char* code;
    const char* make;
    const char* name;
};
// The series with this code, exactly as iDrive writes it ("F20", "RR31"); null for any other.
const CarSeries* findCarSeries(const std::string& code);
bool validCarYear(const std::string& s);
// Accepted id: a UUID as /proc/sys/kernel/random/uuid prints it, 8-4-4-4-12 lowercase hex digits.
bool validVehicleId(const std::string& s);

// The steps of buildCarServiceDiscovery, in its order; each prints what it changed.
// The identity strings replaced, at the top level and in the head unit info (field 17); everything else untouched.
std::vector<uint8_t> buildServiceDiscovery(const uint8_t* base, size_t baseLen, const SdrIdentity& id);
// Main display (channel 6): the canvas's resolution and density, and a bottom inset that keeps the phone's UI in the
// rows the panel shows.
bool tuneMainDisplay(std::vector<uint8_t>& sdr, const Canvas& canvas);
// The touchscreen (channel 7) as large as the canvas.
bool declareTouchscreen(std::vector<uint8_t>& sdr, const Canvas& canvas);
// The driver's side (top-level field 6) and the car's one fuel with no EV connector (the sensor service's fields 3
// and 4). The template describes a left-hand drive car that takes unleaded and electricity from a Tesla supercharger.
// False when the description is malformed, and it is then left as it was.
bool describeCar(std::vector<uint8_t>& sdr, const SdrIdentity& id);
// No media sink (channel 9): media plays over the car's Bluetooth. The phone refuses the session without an audio
// sink or without the microphone, so 8 (speech and guidance) and 14 stay. Returns the descriptors removed.
int removeMediaChannel(std::vector<uint8_t>& sdr);
// The channel 8 sink as SYSTEM_AUDIO instead of GUIDANCE: with neither a guidance nor a media sink the phone mixes
// the prompts into its own audio, which plays over Bluetooth. Returns the fields changed (0 or 1).
int retypeGuidanceSink(std::vector<uint8_t>& sdr);

// The platform's last word on the description, after removeMediaChannel (platform/evo_sdr_sensors.cpp registers
// one: the sensor list cut to what the car feeds). Returns what it removed.
using DiscoveryTrim = int (*)(std::vector<uint8_t>& sdr);
void setDiscoveryTrim(DiscoveryTrim trim);

// What the phone is told about this car: the template adapted by every step above. vehicleFile: the model
// and year (SdrIdentity::loadVehicleFile), given by the platform; null keeps the reference car. wifiAudio: the
// platform plays the phone's sound itself, so the template's sinks stay as they are (media on channel 9, 48 kHz
// stereo; guidance on channel 8, 16 kHz mono first) and neither removeMediaChannel nor retypeGuidanceSink runs.
// The car's Bluetooth address is never in it: told it, Android Auto takes the phone's Bluetooth over (pairing
// waited for a code from the car, reconnection never finished; reference car, 14 November 2026). canvas: the picture
// the phone draws (core/canvas.hpp).
std::vector<uint8_t> buildCarServiceDiscovery(const uint8_t* base, size_t baseLen, const char* vehicleFile,
                                              bool wifiAudio = false, const Canvas& canvas = canvasFor(false));

}  // namespace aa

#endif  // AA_ENTRYNAV_CORE_SDR_BUILDER_HPP
