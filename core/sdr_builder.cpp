// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Builds the car's ServiceDiscoveryResponse: identity strings, main display, touchscreen and audio channels.
#include "sdr_builder.hpp"

#include <cstdio>

#include "aa_protocol.hpp"
#include "pb.hpp"

namespace aa {
namespace {

typedef std::vector<uint8_t> Bytes;

// Field numbers along the paths edited here.
const uint32_t kServiceField = 1;        // top level: one per channel
const uint32_t kHeadUnitInfoField = 17;  // top level: copies of the identity strings
const uint32_t kChannelIdField = 1;      // in a service
const uint32_t kMediaSinkField = 3;      // in a service
const uint32_t kInputSourceField = 4;    // in a service
const uint32_t kAudioTypeField = 2;      // in a media sink
const uint32_t kVideoConfigField = 4;    // in a media sink
const uint32_t kResolutionField = 1;     // in a video config: 1 for 800x480, 2 for 1280x720, 3 for 1920x1080
const uint32_t kDensityField = 5;        // in a video config, with kRealDensityField
const uint32_t kRealDensityField = 9;
const uint32_t kUiConfigField = 11;    // in a video config: the insets as its field 1, then theme and the like
const uint32_t kInsetsField = 1;       // top, bottom, left, right
const uint32_t kTouchscreenField = 2;  // in an input source
const uint32_t kWidthField = 1;        // in a touchscreen, with kHeightField
const uint32_t kHeightField = 2;
const uint32_t kDriverPositionField = 6;  // top level: 0 left, 1 right
const uint32_t kSensorSourceField = 2;    // in a service
const uint32_t kFuelTypeField = 3;        // in a sensor source, repeated
const uint32_t kEvConnectorField = 4;     // in a sensor source, repeated

const uint64_t kGuidanceAudio = 1;
const uint8_t kSystemAudio = 2;

// Each identity string: its field at the top level, its field in the head unit info, its value.
struct IdentityField {
    uint32_t top;
    uint32_t info;
    std::string SdrIdentity::*value;
};
const IdentityField kIdentity[] = {{2, 1, &SdrIdentity::carMake}, {3, 2, &SdrIdentity::carModel},
                                   {4, 3, &SdrIdentity::carYear}, {5, 4, &SdrIdentity::vehicleId},
                                   {7, 5, &SdrIdentity::huMake},  {8, 6, &SdrIdentity::huModel},
                                   {9, 7, &SdrIdentity::swBuild}, {10, 8, &SdrIdentity::swVersion}};

// A string field of the identity, replaced by its value; false for any other field.
bool replaceIdentity(const pb::Field& field, Bytes& out, const SdrIdentity& id, uint32_t IdentityField::*column) {
    if (field.type != pb::kBytes) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kIdentity) / sizeof(kIdentity[0]); ++i) {
        if (kIdentity[i].*column == field.number) {
            pb::appendString(out, field.number, id.*(kIdentity[i].value));
            return true;
        }
    }
    return false;
}

// A sub-message rewritten by `edit` and written back under its own number; false (nothing written) when malformed.
template <typename Edit>
bool rewriteInner(const pb::Field& field, Bytes& out, Edit edit) {
    Bytes inner;
    if (!pb::rewriteMessage(field.data, field.size, inner, edit)) {
        return false;
    }
    pb::appendMessage(out, field.number, inner);
    return true;
}

// The channel a top-level service describes; none when the field is not a service.
bool isChannel(const pb::Field& field, uint8_t channel) {
    uint64_t id = 0;
    return pb::isMessage(field, kServiceField) && pb::findVarint(field.data, field.size, kChannelIdField, id) &&
           id == channel;
}

// The services of one channel rewritten by `edit`, the rest copied. False when the description is malformed; else
// `changed` tells whether such a service was rewritten.
template <typename Edit>
bool rewriteChannel(Bytes& sdr, uint8_t channel, Edit edit, bool& changed) {
    Bytes out;
    changed = false;
    const bool ok = pb::rewriteMessage(sdr.data(), sdr.size(), out, [&](const pb::Field& service, Bytes& to) -> bool {
        const bool done = isChannel(service, channel) && rewriteInner(service, to, edit);
        changed = changed || done;
        return done;
    });
    if (ok && changed) {
        sdr.swap(out);
    }
    return ok;
}

// The video config of the main display: both densities set, and the UI config written last, its insets replaced.
bool tuneVideoConfig(const pb::Field& config, Bytes& out, const Canvas& canvas) {
    Bytes ui;
    Bytes body;
    const bool ok = pb::rewriteMessage(config.data, config.size, body, [&](const pb::Field& field, Bytes& to) -> bool {
        if (field.type == pb::kVarint && (field.number == kDensityField || field.number == kRealDensityField)) {
            pb::appendUint(to, field.number, canvas.density);
            return true;
        }
        if (field.type == pb::kVarint && field.number == kResolutionField) {
            pb::appendUint(to, field.number, canvas.resolution);
            return true;
        }
        if (pb::isMessage(field, kUiConfigField)) {
            ui.assign(field.data, field.data + field.size);  // rebuilt below
            return true;
        }
        return false;
    });
    if (!ok) {
        return false;
    }
    Bytes insets;
    pb::appendUint(insets, 1, 0);
    pb::appendUint(insets, 2, canvas.height - canvas.shownHeight);
    pb::appendUint(insets, 3, 0);
    pb::appendUint(insets, 4, 0);
    Bytes rebuilt;
    pb::appendMessage(rebuilt, kInsetsField, insets);
    // The template's other UI settings stay, after the insets.
    pb::rewriteMessage(ui.data(), ui.size(), rebuilt,
                       [](const pb::Field& field, Bytes&) -> bool { return field.number == kInsetsField; });
    pb::appendMessage(body, kUiConfigField, rebuilt);
    pb::appendMessage(out, config.number, body);
    return true;
}

}  // namespace

SdrIdentity::SdrIdentity()
    : carMake("BMW"),
      carModel("M340i"),
      carYear("2019"),
      // The phone keeps its per-car settings under this id: never change it.
      vehicleId("3b9e7d52-1c4f-4a8e-b6d0-5f2a9c81e4b3"),
      huMake("BMW"),
      huModel("EntryNav2"),
      swBuild("aa-entrynav"),
      swVersion("1.3.33"),
      rightHandDrive(false),
      fuel(kFuelPetrol) {}

namespace {

const size_t kMaxCarModel = 32;

// Every series code in the head unit's coding (libhmicoding.so, BAUREIHE), 202306092158. A null name: not known
// which cars the code covers.
const CarSeries kCarSeries[] = {
    {"F01", "BMW", "7 Series"},
    {"F02", "BMW", "7 Series"},
    {"F03", "BMW", "7 Series"},
    {"F04", "BMW", "7 Series"},
    {"F06", "BMW", "6 Series Gran Coupe"},
    {"F07", "BMW", "5 Series GT"},
    {"F10", "BMW", "5 Series"},
    {"F11", "BMW", "5 Series"},
    {"F12", "BMW", "6 Series"},
    {"F13", "BMW", "6 Series"},
    {"F15", "BMW", "X5"},
    {"F16", "BMW", "X6"},
    {"F18", "BMW", "5 Series"},
    {"F20", "BMW", "1 Series"},
    {"F21", "BMW", "1 Series"},
    {"F22", "BMW", "2 Series"},
    {"F23", "BMW", "2 Series"},
    {"F25", "BMW", "X3"},
    {"F26", "BMW", "X4"},
    {"F30", "BMW", "3 Series"},
    {"F31", "BMW", "3 Series"},
    {"F32", "BMW", "4 Series"},
    {"F33", "BMW", "4 Series"},
    {"F34", "BMW", "3 Series GT"},
    {"F35", "BMW", "3 Series"},
    {"F36", "BMW", "4 Series Gran Coupe"},
    {"F39", "BMW", "X2"},
    {"F40", "BMW", "1 Series"},
    {"F44", "BMW", "2 Series Gran Coupe"},
    {"F45", "BMW", "2 Series Active Tourer"},
    {"F46", "BMW", "2 Series Gran Tourer"},
    {"F47", "BMW", nullptr},
    {"F48", "BMW", "X1"},
    {"F49", "BMW", "X1"},
    {"F52", "BMW", "1 Series"},
    {"F54", "MINI", "Clubman"},
    {"F55", "MINI", "Hatch"},
    {"F56", "MINI", "Hatch"},
    {"F57", "MINI", "Convertible"},
    {"F60", "MINI", "Countryman"},
    {"F80", "BMW", "M3"},
    {"F82", "BMW", "M4"},
    {"F83", "BMW", "M4"},
    {"F85", "BMW", "X5 M"},
    {"F86", "BMW", "X6 M"},
    {"F87", "BMW", "M2"},
    {"F88", "BMW", nullptr},
    {"F90", "BMW", "M5"},
    {"F91", "BMW", "M8"},
    {"F92", "BMW", "M8"},
    {"F97", "BMW", "X3 M"},
    {"F98", "BMW", "X4 M"},
    {"G01", "BMW", "X3"},
    {"G02", "BMW", "X4"},
    {"G05", "BMW", "X5"},
    {"G06", "BMW", "X6"},
    {"G07", "BMW", "X7"},
    {"G08", "BMW", "iX3"},
    {"G11", "BMW", "7 Series"},
    {"G12", "BMW", "7 Series"},
    {"G14", "BMW", "8 Series"},
    {"G15", "BMW", "8 Series"},
    {"G16", "BMW", "8 Series Gran Coupe"},
    {"G20", "BMW", "3 Series"},
    {"G21", "BMW", "3 Series"},
    {"G22", "BMW", "4 Series"},
    {"G23", "BMW", "4 Series"},
    {"G24", "BMW", nullptr},
    {"G26", "BMW", "4 Series Gran Coupe"},
    {"G28", "BMW", "3 Series"},
    {"G29", "BMW", "Z4"},
    {"G30", "BMW", "5 Series"},
    {"G31", "BMW", "5 Series"},
    {"G32", "BMW", "6 Series GT"},
    {"G38", "BMW", "5 Series"},
    {"I01", "BMW", "i3"},
    {"I12", "BMW", "i8"},
    {"I15", "BMW", "i8"},
    {"J29", "Toyota", "Supra"},
    {"RR04", "Rolls-Royce", "Ghost"},
    {"RR05", "Rolls-Royce", "Wraith"},
    {"RR06", "Rolls-Royce", "Dawn"},
    {"RR11", "Rolls-Royce", nullptr},
    {"RR12", "Rolls-Royce", nullptr},
    {"RR14", "Rolls-Royce", nullptr},
    {"RR21", "Rolls-Royce", "Ghost"},
    {"RR22", "Rolls-Royce", nullptr},
    {"RR24", "Rolls-Royce", nullptr},
    {"RR25", "Rolls-Royce", "Spectre"},
    {"RR31", "Rolls-Royce", "Cullinan"},
};

// The model the phone shows for this series: the one written in the file, else the series name, else the code. The
// code goes in brackets after a name unless the name has a bracket already or would grow too long.
std::string modelOfSeries(const std::string& written, const CarSeries& series) {
    const std::string base = !written.empty() ? written : series.name ? series.name : "";
    if (base.empty()) {
        return series.code;
    }
    const std::string tagged = base + " (" + series.code + ")";
    return base.find('(') != std::string::npos || tagged.size() > kMaxCarModel ? base : tagged;
}

}  // namespace

const CarSeries* findCarSeries(const std::string& code) {
    for (size_t i = 0; i < sizeof(kCarSeries) / sizeof(kCarSeries[0]); ++i) {
        if (code == kCarSeries[i].code) {
            return &kCarSeries[i];
        }
    }
    return nullptr;
}

bool validCarModel(const std::string& s) {
    if (s.empty() || s.size() > kMaxCarModel) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                             c == '.' || c == '_' || c == '-' || c == '(' || c == ')';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool validCarYear(const std::string& s) {
    bool ok = s.size() == 4 && (s[0] == '1' || s[0] == '2');
    for (size_t i = 1; ok && i < 4; ++i) {
        ok = s[i] >= '0' && s[i] <= '9';
    }
    return ok;
}

bool validVehicleId(const std::string& s) {
    if (s.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const bool ok = dash ? c == '-' : ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool SdrIdentity::loadVehicleFile(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return false;
    }
    char text[64];
    std::string written;  // the model= line
    const CarSeries* series = nullptr;
    while (fgets(text, sizeof(text), file)) {
        std::string line(text);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) {
            line.erase(line.size() - 1);
        }
        if (line.compare(0, 6, "model=") == 0 && validCarModel(line.substr(6))) {
            written = line.substr(6);
            carModel = written;
        } else if (line.compare(0, 7, "series=") == 0 && findCarSeries(line.substr(7))) {
            series = findCarSeries(line.substr(7));
        } else if (line.compare(0, 5, "year=") == 0 && validCarYear(line.substr(5))) {
            carYear = line.substr(5);
        } else if (line.compare(0, 3, "id=") == 0 && validVehicleId(line.substr(3))) {
            vehicleId = line.substr(3);
        } else if (line == "side=left" || line == "side=right") {
            rightHandDrive = line == "side=right";
        } else if (line == "fuel=petrol" || line == "fuel=diesel") {
            fuel = line == "fuel=diesel" ? kFuelDiesel : kFuelPetrol;
        }
    }
    fclose(file);
    if (series) {
        carMake = series->make;
        carModel = modelOfSeries(written, *series);
    }
    return true;
}

std::vector<uint8_t> buildServiceDiscovery(const uint8_t* base, size_t baseLen, const SdrIdentity& id) {
    Bytes out;
    const bool ok = pb::rewriteMessage(base, baseLen, out, [&](const pb::Field& field, Bytes& to) -> bool {
        if (pb::isMessage(field, kHeadUnitInfoField)) {
            return rewriteInner(field, to, [&](const pb::Field& info, Bytes& into) -> bool {
                return replaceIdentity(info, into, id, &IdentityField::info);
            });
        }
        return replaceIdentity(field, to, id, &IdentityField::top);
    });
    return ok ? out : Bytes(base, base + baseLen);
}

bool tuneMainDisplay(std::vector<uint8_t>& sdr, const Canvas& canvas) {
    bool changed = false;
    const bool ok = rewriteChannel(
        sdr, channel::kVideo,
        [&](const pb::Field& field, Bytes& to) -> bool {
            return pb::isMessage(field, kMediaSinkField) &&
                   rewriteInner(field, to, [&](const pb::Field& sink, Bytes& into) -> bool {
                       return pb::isMessage(sink, kVideoConfigField) && tuneVideoConfig(sink, into, canvas);
                   });
        },
        changed);
    if (ok && changed) {
        printf("[SDR] main VideoConfig: %ux%u dpi=%u insets=(0,%u,0,0)\n", canvas.width, canvas.height, canvas.density,
               canvas.height - canvas.shownHeight);
    }
    return ok && changed;
}

bool declareTouchscreen(std::vector<uint8_t>& sdr, const Canvas& canvas) {
    bool changed = false;
    const bool ok = rewriteChannel(
        sdr, channel::kInput,
        [&](const pb::Field& field, Bytes& to) -> bool {
            return pb::isMessage(field, kInputSourceField) &&
                   rewriteInner(field, to, [&](const pb::Field& source, Bytes& into) -> bool {
                       return pb::isMessage(source, kTouchscreenField) &&
                              rewriteInner(source, into, [&](const pb::Field& size, Bytes& sized) -> bool {
                                  if (size.type != pb::kVarint ||
                                      (size.number != kWidthField && size.number != kHeightField)) {
                                      return false;
                                  }
                                  pb::appendUint(sized, size.number,
                                                 size.number == kWidthField ? canvas.width : canvas.height);
                                  return true;
                              });
                   });
        },
        changed);
    if (ok && changed) {
        printf("[SDR] touchscreen declared %ux%u\n", canvas.width, canvas.height);
    }
    return ok && changed;
}

bool describeCar(std::vector<uint8_t>& sdr, const SdrIdentity& id) {
    // The sensor source: its fuel types and EV connectors dropped, the car's one fuel written last.
    const auto fuelOnly = [&](const pb::Field& source, Bytes& to) -> bool {
        Bytes body;
        if (!pb::rewriteMessage(source.data, source.size, body, [](const pb::Field& field, Bytes&) -> bool {
                return field.number == kFuelTypeField || field.number == kEvConnectorField;
            })) {
            return false;
        }
        pb::appendUint(body, kFuelTypeField, id.fuel);
        pb::appendMessage(to, source.number, body);
        return true;
    };
    bool sideSet = false, fuelSet = false;
    Bytes out;
    const bool ok = pb::rewriteMessage(sdr.data(), sdr.size(), out, [&](const pb::Field& field, Bytes& to) -> bool {
        if (field.type == pb::kVarint && field.number == kDriverPositionField) {
            pb::appendUint(to, kDriverPositionField, id.rightHandDrive ? 1 : 0);
            sideSet = true;
            return true;
        }
        if (isChannel(field, channel::kSensors)) {
            return rewriteInner(field, to, [&](const pb::Field& part, Bytes& into) -> bool {
                const bool done = pb::isMessage(part, kSensorSourceField) && fuelOnly(part, into);
                fuelSet = fuelSet || done;
                return done;
            });
        }
        return false;
    });
    if (!ok) {
        return false;
    }
    if (!sideSet) {
        pb::appendUint(out, kDriverPositionField, id.rightHandDrive ? 1 : 0);
    }
    sdr.swap(out);
    printf("[SDR] driver's side: %s; fuel: %s, no EV connector%s\n", id.rightHandDrive ? "right" : "left",
           id.fuel == kFuelDiesel ? "diesel" : "petrol", fuelSet ? "" : " (no sensor source: fuel not told)");
    return true;
}

int removeMediaChannel(std::vector<uint8_t>& sdr) {
    int removed = 0;
    Bytes out;
    const bool ok = pb::rewriteMessage(sdr.data(), sdr.size(), out, [&](const pb::Field& service, Bytes&) -> bool {
        if (!isChannel(service, channel::kMediaAudio)) {
            return false;
        }
        printf("[SDR] dropped channel %u (media over Bluetooth)\n", channel::kMediaAudio);
        ++removed;
        return true;
    });
    if (!ok) {
        return 0;
    }
    if (removed) {
        sdr.swap(out);
    }
    return removed;
}

// In place: GUIDANCE (1) and SYSTEM_AUDIO (2) are both one-byte values.
int retypeGuidanceSink(std::vector<uint8_t>& sdr) {
    int changed = 0;
    pb::FieldReader services(sdr.data(), sdr.size());
    pb::Field service;
    while (services.next(service)) {
        if (!isChannel(service, channel::kGuidanceAudio)) {
            continue;
        }
        pb::FieldReader parts(service.data, service.size);
        pb::Field sink;
        while (parts.next(sink)) {
            if (!pb::isMessage(sink, kMediaSinkField)) {
                continue;
            }
            pb::FieldReader settings(sink.data, sink.size);
            pb::Field type;
            while (settings.next(type)) {
                if (type.type == pb::kVarint && type.number == kAudioTypeField && type.value == kGuidanceAudio &&
                    type.end - type.begin == 2) {
                    sdr[static_cast<size_t>(sink.data - sdr.data()) + type.end - 1] = kSystemAudio;
                    ++changed;
                    printf("[SDR] channel 8 audio sink: GUIDANCE -> SYSTEM_AUDIO (nav prompts over Bluetooth)\n");
                }
            }
        }
    }
    return changed;
}

namespace {
DiscoveryTrim g_trim = nullptr;
}  // namespace

void setDiscoveryTrim(DiscoveryTrim trim) {
    g_trim = trim;
}

std::vector<uint8_t> buildCarServiceDiscovery(const uint8_t* base, size_t baseLen, const char* vehicleFile,
                                              bool wifiAudio, const Canvas& canvas) {
    SdrIdentity identity;
    if (vehicleFile) {
        identity.loadVehicleFile(vehicleFile);
    }
    std::vector<uint8_t> sdr = buildServiceDiscovery(base, baseLen, identity);
    printf("[SDR] ServiceDiscovery identity: %s %s %s\n", identity.carMake.c_str(), identity.carModel.c_str(),
           identity.carYear.c_str());
    tuneMainDisplay(sdr, canvas);
    declareTouchscreen(sdr, canvas);
    describeCar(sdr, identity);
    if (wifiAudio) {
        printf("[SDR] sound over Wi-Fi: media sink (channel %u) and guidance sink (channel %u) kept\n",
               channel::kMediaAudio, channel::kGuidanceAudio);
    } else {
        removeMediaChannel(sdr);
    }
    if (g_trim) {
        g_trim(sdr);
    }
    if (!wifiAudio) {
        retypeGuidanceSink(sdr);
    }
    return sdr;
}

}  // namespace aa
