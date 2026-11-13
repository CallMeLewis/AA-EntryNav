// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The ServiceDiscoveryResponse that describes this car to the phone, built from Google's DHU template.
#ifndef AA_ENTRYNAV_CORE_SDR_BUILDER_HPP
#define AA_ENTRYNAV_CORE_SDR_BUILDER_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace aa {

// What the phone is told about the car and the head unit.
struct SdrIdentity {
    std::string carMake, carModel, carYear, vehicleId;
    std::string huMake, huModel, swBuild, swVersion;
    SdrIdentity();  // this car and head unit
    // Model, year and id of a car other than the reference one, written at installation ("model=320d xDrive",
    // "year=2021", "id=<uuid>", one per line). A missing file or an invalid line keeps the reference values. False if
    // unreadable. Without an id= line the reference id stays, so a phone keeps the settings it already has.
    bool loadVehicleFile(const char* path);
};
// Accepted model and year: 1 to 24 of A-Z a-z 0-9 space . _ -, and 1xxx or 2xxx.
bool validCarModel(const std::string& s);
bool validCarYear(const std::string& s);
// Accepted id: a UUID as /proc/sys/kernel/random/uuid prints it, 8-4-4-4-12 lowercase hex digits.
bool validVehicleId(const std::string& s);

// The picture the phone draws. The panel is 1280x480 and shows the canvas's top shownHeight rows, the full width:
//   720p    1280x720 shown 1:1, density 140 (seen on the car: the rail icons no longer overlap)
//   1080p   1920x1080, its top 1920x720 shrunk onto the panel by the car's compositor. Density 210 keeps every
//           button the same size on the panel; the phone draws 1.5 times the detail, so diagonal edges come out
//           smoother and colours bleed less around thin lines.
struct Canvas {
    uint32_t resolution;     // the VideoConfig's code: 2 for 1280x720, 3 for 1920x1080
    uint32_t width, height;  // also the touchscreen's size: touches arrive in canvas pixels
    uint32_t shownHeight;    // rows shown on the panel; the rest is the bottom inset
    uint32_t density;
};
const Canvas& canvasFor(bool hd);

// The steps of buildCarServiceDiscovery, in its order; each prints what it changed.
// The identity strings replaced, at the top level and in the head unit info (field 17); everything else untouched.
std::vector<uint8_t> buildServiceDiscovery(const uint8_t* base, size_t baseLen, const SdrIdentity& id);
// Main display (channel 6): the canvas's resolution and density, and a bottom inset that keeps the phone's UI in the
// rows the panel shows.
bool tuneMainDisplay(std::vector<uint8_t>& sdr, const Canvas& canvas);
// The touchscreen (channel 7) as large as the canvas.
bool declareTouchscreen(std::vector<uint8_t>& sdr, const Canvas& canvas);
// No media sink (channel 9): media plays over the car's Bluetooth. The phone refuses the session without an audio
// sink or without the microphone, so 8 (speech and guidance) and 14 stay. Returns the descriptors removed.
int removeMediaChannel(std::vector<uint8_t>& sdr);
// The channel 8 sink as SYSTEM_AUDIO instead of GUIDANCE: with neither a guidance nor a media sink the phone mixes
// the prompts into its own audio, which plays over Bluetooth. Returns the fields changed (0 or 1).
int retypeGuidanceSink(std::vector<uint8_t>& sdr);

// A car Bluetooth address as BlueZ prints it: six pairs of hex digits joined by colons, "00:1A:7D:DA:71:13".
bool validBluetoothAddress(const std::string& s);
// The car's Bluetooth as a service of its own (channel 15) right after the template's last one: its address, in
// capitals, and the pairing methods the car's stack offers (numeric comparison, then PIN). The phone opens the
// channel, asks whether it is paired with the car, and pairs when it is not. False, nothing added, when the address is
// invalid, the description malformed or the service already there.
bool addBluetoothService(std::vector<uint8_t>& sdr, const std::string& carAddress);

// The platform's last word on the description, after removeMediaChannel (platform/evo_sdr_sensors.cpp registers
// one: the sensor list cut to what the car feeds). Returns what it removed.
using DiscoveryTrim = int (*)(std::vector<uint8_t>& sdr);
void setDiscoveryTrim(DiscoveryTrim trim);

// What the phone is told about this car: the template adapted by every step above. vehicleFile: the model
// and year (SdrIdentity::loadVehicleFile), given by the platform; null keeps the reference car. wifiAudio: the
// platform plays the phone's sound itself, so the template's sinks stay as they are (media on channel 9, 48 kHz
// stereo; guidance on channel 8, 16 kHz mono first) and neither removeMediaChannel nor retypeGuidanceSink runs.
// bluetoothAddress: the car's, for addBluetoothService; empty (as on Wi-Fi, where the phone is already connected to
// the car's Bluetooth) leaves the description without it. hdVideo: the 1080p canvas, else 720p (canvasFor).
std::vector<uint8_t> buildCarServiceDiscovery(const uint8_t* base, size_t baseLen, const char* vehicleFile,
                                              bool wifiAudio = false,
                                              const std::string& bluetoothAddress = std::string(),
                                              bool hdVideo = false);

}  // namespace aa

#endif  // AA_ENTRYNAV_CORE_SDR_BUILDER_HPP
