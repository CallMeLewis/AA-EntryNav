// Copyright (c) 2026 CallMeLewis
// Android Open Accessory requests, USB descriptors and the bus's device list (see usb_aoa.hpp).
#include "usb_aoa.hpp"

#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace evo {

const AoaIdentity kAndroidAutoIdentity = {
    "Android", "Android Auto", "Android Auto", "2.0.1", "https://developer.android.com/auto/index.html", "HU-AAAAAA001",
};

namespace {

const uint8_t kVendorIn = 0xC0, kVendorOut = 0x40;
const uint8_t kGetProtocol = 51, kSendString = 52, kStart = 53;
const unsigned kControlMs = 1000;
const uint8_t kConfiguration = 2, kInterface = 4, kEndpoint = 5;  // descriptor types

// Calls visit(type, bytes, length) for each descriptor after the 18-byte device descriptor.
template <typename Visit>
void walk(const uint8_t* d, size_t size, Visit visit) {
    size_t at = size >= 18 && d[1] == 1 ? d[0] : 0;
    while (at + 2 <= size) {
        const size_t length = d[at];
        if (length < 2 || at + length > size) {
            return;
        }
        visit(d[at + 1], d + at, length);
        at += length;
    }
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    uint8_t buffer[1024];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0 && out.size() < 65536) {
        out.insert(out.end(), buffer, buffer + n);
    }
    fclose(f);
    return true;
}

// A sysfs number, in the base it is written in.
bool readNumber(const std::string& path, int base, unsigned& out) {
    std::vector<uint8_t> text;
    if (!readFile(path, text) || text.empty()) {
        return false;
    }
    text.push_back(0);
    char* end = nullptr;
    const unsigned long value = strtoul(reinterpret_cast<const char*>(&text[0]), &end, base);
    if (end == reinterpret_cast<const char*>(&text[0])) {
        return false;
    }
    out = static_cast<unsigned>(value);
    return true;
}

}  // namespace

bool findAccessoryInterface(const uint8_t* descriptors, size_t size, AccessoryInterface& out) {
    AccessoryInterface current = {0, 0, 0, 0};
    uint8_t configuration = 0;
    bool inside = false, found = false;
    walk(descriptors, size, [&](uint8_t type, const uint8_t* d, size_t length) {
        if (found) {
            return;
        }
        if (type == kConfiguration && length >= 6) {
            configuration = d[5];
            inside = false;
        } else if (type == kInterface && length >= 9) {
            inside = d[5] == 0xFF && d[6] == 0xFF && d[7] == 0x00;
            current.configuration = configuration;
            current.number = d[2];
            current.in = current.out = 0;
        } else if (type == kEndpoint && length >= 7 && inside && (d[3] & 3) == 2) {  // bulk
            if (d[2] & 0x80) {
                current.in = current.in ? current.in : d[2];
            } else {
                current.out = current.out ? current.out : d[2];
            }
            if (current.in && current.out) {
                out = current;
                found = true;
            }
        }
    });
    return found;
}

bool worthAskingForAccessory(uint16_t vendor, const uint8_t* descriptors, size_t size) {
    if (vendor == kAppleVendor || size < 18 || descriptors[1] != 1 || descriptors[4] == 0x09) {
        return false;  // Apple's, no device descriptor, or a hub
    }
    bool phoneLike = false, other = false;
    walk(descriptors, size, [&](uint8_t type, const uint8_t* d, size_t length) {
        if (type != kInterface || length < 9) {
            return;
        }
        const uint8_t cls = d[5];
        if (cls == 0xFF || cls == 0x06) {  // vendor specific, or picture/file transfer (MTP, PTP)
            phoneLike = true;
        } else if (cls == 0x08 || cls == 0x03 || cls == 0x01 || cls == 0x09) {  // storage, keyboard, sound, hub
            other = true;
        }
    });
    return phoneLike && !other;
}

int accessoryProtocol(UsbIo& io) {
    uint8_t version[2] = {0, 0};
    const int rc = io.control(kVendorIn, kGetProtocol, 0, 0, version, sizeof(version), kControlMs);
    if (rc < 0) {
        return rc;
    }
    if (rc != 2) {
        return -EPROTO;
    }
    const int value = version[0] | (version[1] << 8);
    return value >= 1 ? value : -EPROTO;
}

bool startAccessory(UsbIo& io, const AoaIdentity& identity) {
    const char* const texts[6] = {identity.manufacturer, identity.model, identity.description,
                                  identity.version,      identity.uri,   identity.serial};
    for (uint16_t i = 0; i < 6; ++i) {
        std::string text(texts[i] ? texts[i] : "");
        std::vector<uint8_t> bytes(text.begin(), text.end());
        bytes.push_back(0);  // the phone reads a C string
        const int rc =
            io.control(kVendorOut, kSendString, 0, i, &bytes[0], static_cast<uint16_t>(bytes.size()), kControlMs);
        if (rc != static_cast<int>(bytes.size())) {
            return false;
        }
    }
    return io.control(kVendorOut, kStart, 0, 0, nullptr, 0, kControlMs) == 0;
}

std::vector<UsbDeviceInfo> listUsbDevices(const char* root) {
    std::vector<UsbDeviceInfo> devices;
    DIR* dir = opendir(root);
    if (!dir) {
        return devices;
    }
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        // Devices are "bus-port[.port...]"; "usbN" are the root hubs and "x-y:c.i" the interfaces.
        if (name.empty() || name[0] < '0' || name[0] > '9' || name.find(':') != std::string::npos ||
            name.find('-') == std::string::npos) {
            continue;
        }
        const std::string base = std::string(root) + "/" + name;
        UsbDeviceInfo info;
        unsigned vendor = 0, product = 0;
        if (!readNumber(base + "/idVendor", 16, vendor) || !readNumber(base + "/idProduct", 16, product) ||
            !readNumber(base + "/busnum", 10, info.bus) || !readNumber(base + "/devnum", 10, info.device) ||
            !readFile(base + "/descriptors", info.descriptors)) {
            continue;  // unplugged while it was read
        }
        info.name = name;
        info.vendor = static_cast<uint16_t>(vendor);
        info.product = static_cast<uint16_t>(product);
        devices.push_back(info);
    }
    closedir(dir);
    return devices;
}

std::string interfaceDriver(const UsbDeviceInfo& device, const AccessoryInterface& interface, const char* root) {
    char link[256], target[256];
    snprintf(link, sizeof(link), "%s/%s:%u.%u/driver", root, device.name.c_str(),
             static_cast<unsigned>(interface.configuration), static_cast<unsigned>(interface.number));
    const ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) {
        return std::string();
    }
    target[n] = 0;
    const char* slash = strrchr(target, '/');
    return std::string(slash ? slash + 1 : target);
}

}  // namespace evo
