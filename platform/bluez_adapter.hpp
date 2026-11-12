// Copyright (c) 2026 CallMeLewis
// Read-only questions to the car's Bluetooth: its address, whether it takes pairings, and whether a phone is paired.
//
// BMW's Bluetooth stack (Alps "Evolution", evo_genivi_bt) serves the BlueZ 4 D-Bus API on the system bus: service
// org.bluez, adapter /org/bluez/hci1 (the path BMW's btmbt uses; hci0 is tried after), org.bluez.Adapter.GetProperties
// (Address, Pairable, Discoverable...), FindDevice(address) and org.bluez.Device.GetProperties (Paired). Root may call
// it (/opt/app/usr/BTMedia/evolution/conf/bluetooth.conf). Nothing here sets a property, pairs or connects: pairing
// stays BMW's, through the iDrive.
#ifndef AA_ENTRYNAV_PLATFORM_BLUEZ_ADAPTER_HPP
#define AA_ENTRYNAV_PLATFORM_BLUEZ_ADAPTER_HPP

#include <string>

namespace evo {

struct BluezAdapter {
    std::string path;     // "/org/bluez/hci1"
    std::string address;  // "00:1A:7D:DA:71:13", as the stack prints it
    bool pairable;        // takes a pairing it did not start (the iDrive's "new device" screen sets it)
    bool discoverable;
    BluezAdapter() : pairable(false), discoverable(false) {}
};

// The car's adapter. False when the stack does not answer (not running, or no such adapter) or gives no address.
bool readBluezAdapter(BluezAdapter& out);

// Is the phone with this address paired with the car? 1 paired, 0 not (unknown to the car, or known and not
// paired), -1 the stack could not be asked. Any case of hex digits.
int bluezPaired(const std::string& adapterPath, const std::string& phoneAddress);

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_BLUEZ_ADAPTER_HPP
