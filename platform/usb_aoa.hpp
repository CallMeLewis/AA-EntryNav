// Copyright (c) 2026 CallMeLewis
// Android Open Accessory: how a phone on a cable is asked to speak Android Auto.
//
// The head unit is the USB host. It asks the phone which accessory protocol it knows (vendor request 51), tells it
// what it is in six texts (52) and starts accessory mode (53). The phone then comes back as Google's accessory device
// (18D1:2D00 to 2D05) with one interface of class FF/FF/00 and two bulk endpoints, over which Android Auto runs
// exactly as it does over TCP. The texts decide which app answers: "Android" / "Android Auto" starts Android Auto.
//
// Seen on a car with iDrive software 202306092158: BMW's own media program (vmostAuxInUSB) puts every Android phone
// into accessory mode for its own app link, as 18D1:2D01, and holds the accessory interface. So a phone already in
// accessory mode is not proof it was asked for Android Auto: the texts are sent again and the interface is taken.
#ifndef AA_ENTRYNAV_PLATFORM_USB_AOA_HPP
#define AA_ENTRYNAV_PLATFORM_USB_AOA_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "usb_io.hpp"

namespace evo {

const uint16_t kGoogleVendor = 0x18D1, kAppleVendor = 0x05AC;
// 18D1:2D00 accessory, 2D01 with adb, 2D02/2D03 audio, 2D04/2D05 accessory with audio.
inline bool isAccessoryId(uint16_t vendor, uint16_t product) {
    return vendor == kGoogleVendor && product >= 0x2D00 && product <= 0x2D05;
}
inline bool hasAccessoryInterface(uint16_t product) {
    return product != 0x2D02 && product != 0x2D03;
}

// The six texts of request 52, in its order.
struct AoaIdentity {
    const char* manufacturer;
    const char* model;
    const char* description;
    const char* version;
    const char* uri;
    const char* serial;
};
extern const AoaIdentity kAndroidAutoIdentity;

// Where the accessory's data flows.
struct AccessoryInterface {
    uint8_t configuration, number, in, out;
};
// descriptors: the device descriptor followed by its configurations, as the kernel's sysfs "descriptors" file gives
// them. The first interface of class FF/FF/00 with a bulk endpoint each way.
bool findAccessoryInterface(const uint8_t* descriptors, size_t size, AccessoryInterface& out);
// A device worth asking for the accessory protocol: not a hub, not Apple's (left to CarPlay), no storage, keyboard
// or sound interface, and at least one interface a phone shows (vendor specific, or the picture/file transfer class).
bool worthAskingForAccessory(uint16_t vendor, const uint8_t* descriptors, size_t size);

// Request 51: the phone's accessory protocol version (1 or 2), or a negative errno (not a phone that knows it).
int accessoryProtocol(UsbIo& io);
// Requests 52 (six times) and 53. False as soon as one is refused; the phone then stays as it was.
bool startAccessory(UsbIo& io, const AoaIdentity& identity);

// One device of the bus, read from sysfs.
struct UsbDeviceInfo {
    std::string name;  // its place on the bus: "3-1", "1-1.2"
    uint16_t vendor, product;
    unsigned bus, device;
    std::vector<uint8_t> descriptors;
    UsbDeviceInfo() : vendor(0), product(0), bus(0), device(0) {}
};
// Every device but the root hubs. root: /sys/bus/usb/devices on the car.
std::vector<UsbDeviceInfo> listUsbDevices(const char* root = "/sys/bus/usb/devices");
// The driver holding one of its interfaces ("usbfs": a program), empty when nobody does.
std::string interfaceDriver(const UsbDeviceInfo& device, const AccessoryInterface& interface,
                            const char* root = "/sys/bus/usb/devices");

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_USB_AOA_HPP
