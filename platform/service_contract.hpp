// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the iDrive app, the supervisor (evo-trial) and the service (aa-entrynav) share: file paths, the control
// channel's name and the version of what goes over it.
//
// The installation procedure (docs/INSTALLATION.md, docs/MAINTENANCE.md) names the same paths: change both together.
// Flare Drive reads them too.
#ifndef AA_ENTRYNAV_PLATFORM_SERVICE_CONTRACT_HPP
#define AA_ENTRYNAV_PLATFORM_SERVICE_CONTRACT_HPP

#include <string.h>

namespace evo {
namespace contract {

// ---- programs, on the system partition ----------------------------------------------------------------------------
constexpr char kSupervisor[] = "/opt/aa/evo-trial";
constexpr char kService[] = "/opt/aa/aa-entrynav";

// ---- settings and logs, on the data partition (kept across restarts) ----------------------------------------------
constexpr char kDataDir[] = "/var/local/data/AA";
constexpr char kAutostart[] = "/var/local/data/AA/autostart";         // present: Android Auto starts with iDrive
constexpr char kEntryDirect[] = "/var/local/data/AA/entry-direct";    // present: the iDrive row opens Android Auto
constexpr char kMaintenance[] = "/var/local/data/AA/maintenance";     // present: the app and the service keep logs
constexpr char kKombiOff[] = "/var/local/data/AA/kombi-off";          // present: nothing is sent to the cluster
constexpr char kKombiShown[] = "/var/local/data/AA/kombi-nav.shown";  // an arrow may still be on the cluster
constexpr char kPhoneGps[] = "/var/local/data/AA/gps-telephone";      // present: the phone keeps its own GPS
constexpr char kVehicle[] = "/var/local/data/AA/vehicle";             // model=... and year=... told to the phone
constexpr char kAudioWifi[] = "/var/local/data/AA/audio-wifi";        // present: sound over Wi-Fi (evo_audio.hpp)
constexpr char kUsb[] = "/var/local/data/AA/usb";  // present: Android Auto also over a cable (usb_link.hpp)
// present: no wireless Android Auto. The car's Bluetooth and Wi-Fi hotspot are left untouched and only the cable is
// used, so it needs kUsb; without kUsb the supervisor starts nothing.
constexpr char kWirelessOff[] = "/var/local/data/AA/wireless-off";
// present: as the phone connects, the iDrive app switches iDrive's sound to Bluetooth audio (hmiapp/bt_audio.hpp). Not
// with kAudioWifi.
constexpr char kSelectBtAudio[] = "/var/local/data/AA/select-bt-audio";
constexpr char kAppLog[] = "/var/local/data/AA/app.log";
constexpr char kAppLogOld[] = "/var/local/data/AA/app.old.log";
constexpr char kAppDisable[] = "/var/local/data/AA/app.disable";  // present: the app stays off (set by hand)
constexpr char kAppGuard[] = "/var/local/data/AA/app-guard";   // crash guard: 3 short iDrive runs and the app stays off
constexpr char kBootGuard[] = "/var/local/data/AA/bootguard";  // automatic starts within 15 minutes

// ---- run time, in /tmp (memory, gone at the next start) -----------------------------------------------------------
constexpr char kServiceDir[] = "/tmp/aa-entrynav-service";
constexpr char kServiceStop[] = "/tmp/aa-entrynav-service/STOP";  // created by the app: the supervisor stops
constexpr char kServiceLog[] = "/tmp/aa-entrynav-service.log";
constexpr char kServiceLogOld[] = "/tmp/aa-entrynav-service.old.log";  // the log's previous megabyte (log_profile.hpp)
constexpr char kStateFile[] = "/tmp/aa-entrynav-state";                // "state pid overlay", written by the service
constexpr char kAppKeyFile[] = "/tmp/aa-entrynav-appkey";              // NAV/MEDIA/COM opened an Android Auto app
constexpr char kDayNightFile[] = "/tmp/aa-entrynav-daynight";          // day or night, written by the app
constexpr char kWirelessLock[] = "/tmp/aa-entrynav-wireless.lock";     // one wireless start at a time
// The service's udev rule while Android Auto over a cable is on (usb_link.hpp: kUsbUdevRuleDir/kUsbUdevRuleName). The
// service removes it as it stops; the supervisor removes it again once the service has ended however it ended, since
// a crash or the forced stop skips the service's own removal, and the rule would keep BMW's media program away from
// every Android phone until the next restart.
constexpr char kUsbUdevRuleFile[] = "/run/udev/rules.d/99-zz-aa-entrynav-usb.rules";

// ---- control messages from the app to the service -----------------------------------------------------------------
// A Unix datagram socket with this abstract name (platform/control_address.hpp): no file and no port. It was UDP port
// 52790 on 127.0.0.1 before, inside the range the head unit hands out to any program that asks for a free port.
constexpr char kControlName[] = "aa-entrynav-control";

// The version of what the app and the service say to each other. The app hands it to the supervisor in its
// environment, and the supervisor starts nothing for an app of another version. After an update the iDrive keeps the
// previous app loaded until the head unit restarts; that app would start the new service and talk to it where it no
// longer listens, so the service would never hear of reverse gear and would stay over the reversing camera. Raise it
// whenever the channel or its messages change in a way the other side would not understand.
constexpr char kProtocolVar[] = "AA_ENTRYNAV_PROTOCOL";
constexpr char kProtocol[] = "2";
constexpr char kProtocolEnv[] = "AA_ENTRYNAV_PROTOCOL=2";  // kProtocolVar=kProtocol, for the app's spawn environment
inline bool appProtocolMatches(const char* value) {
    return value && !strcmp(value, kProtocol);
}

}  // namespace contract
}  // namespace evo

#endif
