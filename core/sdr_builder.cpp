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
const uint32_t kDensityField = 5;        // in a video config, with kRealDensityField
const uint32_t kRealDensityField = 9;
const uint32_t kUiConfigField = 11;         // in a video config: the insets as its field 1, then theme and the like
const uint32_t kInsetsField = 1;            // top, bottom, left, right
const uint32_t kTouchscreenField = 2;       // in an input source
const uint32_t kHeightField = 2;            // in a touchscreen
const uint32_t kBluetoothServiceField = 6;  // in a service
const uint32_t kCarAddressField = 1;        // in a Bluetooth service, with its pairing methods (packed enums)
const uint32_t kPairingMethodsField = 2;
const uint64_t kPairingNumericComparison = 2;
const uint64_t kPairingPin = 4;

const uint32_t kDensity = 140;
const uint32_t kBottomInset = 240;
const uint32_t kTouchHeight = 720;
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
bool tuneVideoConfig(const pb::Field& config, Bytes& out) {
    Bytes ui;
    Bytes body;
    const bool ok = pb::rewriteMessage(config.data, config.size, body, [&](const pb::Field& field, Bytes& to) -> bool {
        if (field.type == pb::kVarint && (field.number == kDensityField || field.number == kRealDensityField)) {
            pb::appendUint(to, field.number, kDensity);
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
    pb::appendUint(insets, 2, kBottomInset);
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
      swVersion("1.3.33") {}

bool validCarModel(const std::string& s) {
    if (s.empty() || s.size() > 24) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                             c == '.' || c == '_' || c == '-';
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
    while (fgets(text, sizeof(text), file)) {
        std::string line(text);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) {
            line.erase(line.size() - 1);
        }
        if (line.compare(0, 6, "model=") == 0 && validCarModel(line.substr(6))) {
            carModel = line.substr(6);
        } else if (line.compare(0, 5, "year=") == 0 && validCarYear(line.substr(5))) {
            carYear = line.substr(5);
        } else if (line.compare(0, 3, "id=") == 0 && validVehicleId(line.substr(3))) {
            vehicleId = line.substr(3);
        }
    }
    fclose(file);
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

bool tuneMainDisplay(std::vector<uint8_t>& sdr) {
    bool changed = false;
    const bool ok = rewriteChannel(
        sdr, channel::kVideo,
        [](const pb::Field& field, Bytes& to) -> bool {
            return pb::isMessage(field, kMediaSinkField) &&
                   rewriteInner(field, to, [](const pb::Field& sink, Bytes& into) -> bool {
                       return pb::isMessage(sink, kVideoConfigField) && tuneVideoConfig(sink, into);
                   });
        },
        changed);
    if (ok && changed) {
        printf("[SDR] main VideoConfig: dpi=%u insets=(0,%u,0,0)\n", kDensity, kBottomInset);
    }
    return ok && changed;
}

bool declareTouchscreen(std::vector<uint8_t>& sdr) {
    bool changed = false;
    const bool ok = rewriteChannel(
        sdr, channel::kInput,
        [](const pb::Field& field, Bytes& to) -> bool {
            return pb::isMessage(field, kInputSourceField) &&
                   rewriteInner(field, to, [](const pb::Field& source, Bytes& into) -> bool {
                       return pb::isMessage(source, kTouchscreenField) &&
                              rewriteInner(source, into, [](const pb::Field& size, Bytes& sized) -> bool {
                                  if (size.type != pb::kVarint || size.number != kHeightField) {
                                      return false;
                                  }
                                  pb::appendUint(sized, kHeightField, kTouchHeight);
                                  return true;
                              });
                   });
        },
        changed);
    if (ok && changed) {
        printf("[SDR] touchscreen declared 1280x%u\n", kTouchHeight);
    }
    return ok && changed;
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

bool validBluetoothAddress(const std::string& s) {
    if (s.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (i % 3 == 2 ? c != ':' : !hex) {
            return false;
        }
    }
    return true;
}

bool addBluetoothService(std::vector<uint8_t>& sdr, const std::string& carAddress) {
    if (!validBluetoothAddress(carAddress)) {
        return false;
    }
    size_t after = 0;  // the end of the last service
    pb::FieldReader reader(sdr.data(), sdr.size());
    pb::Field field;
    while (reader.next(field)) {
        if (isChannel(field, channel::kBluetooth)) {
            return false;
        }
        if (pb::isMessage(field, kServiceField)) {
            after = field.end;
        }
    }
    if (reader.malformed()) {
        return false;
    }
    std::string address(carAddress);
    for (size_t i = 0; i < address.size(); ++i) {
        if (address[i] >= 'a' && address[i] <= 'f') {
            address[i] = static_cast<char>(address[i] - 'a' + 'A');
        }
    }
    Bytes methods;  // packed
    pb::appendVarint(methods, kPairingNumericComparison);
    pb::appendVarint(methods, kPairingPin);
    Bytes bluetooth;
    pb::appendString(bluetooth, kCarAddressField, address);
    pb::appendMessage(bluetooth, kPairingMethodsField, methods);
    Bytes service;
    pb::appendUint(service, kChannelIdField, channel::kBluetooth);
    pb::appendMessage(service, kBluetoothServiceField, bluetooth);
    Bytes entry;
    pb::appendMessage(entry, kServiceField, service);
    sdr.insert(sdr.begin() + static_cast<std::ptrdiff_t>(after), entry.begin(), entry.end());
    printf("[SDR] Bluetooth service (channel %u): car %s, pairing by numeric comparison or PIN\n", channel::kBluetooth,
           address.c_str());
    return true;
}

std::vector<uint8_t> buildCarServiceDiscovery(const uint8_t* base, size_t baseLen, const char* vehicleFile,
                                              bool wifiAudio, const std::string& bluetoothAddress) {
    SdrIdentity identity;
    if (vehicleFile) {
        identity.loadVehicleFile(vehicleFile);
    }
    std::vector<uint8_t> sdr = buildServiceDiscovery(base, baseLen, identity);
    printf("[SDR] ServiceDiscovery identity: %s %s %s (BT channel: %s)\n", identity.carMake.c_str(),
           identity.carModel.c_str(), identity.carYear.c_str(), bluetoothAddress.empty() ? "none" : "added below");
    tuneMainDisplay(sdr);
    declareTouchscreen(sdr);
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
    if (!bluetoothAddress.empty() && !addBluetoothService(sdr, bluetoothAddress)) {
        printf("[SDR] Bluetooth service not added: car address \"%s\" unusable\n", bluetoothAddress.c_str());
    }
    return sdr;
}

}  // namespace aa
