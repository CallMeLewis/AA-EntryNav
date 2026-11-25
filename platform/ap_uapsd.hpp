// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's access point offers no U-APSD to the phone that joins it.
//
// wland writes uapsd_advertisement_enabled=1 into hostapd's configuration, so the access point offers U-APSD, a power
// saving mode, to the phones that join it. Upstream saw it on an X1 F48 (EntryNav2-Android-Auto #138, #160): the
// WL18xx Wi-Fi chip held the phone's frames back for seconds (its ps_poll_upsd_timeouts counter reached 1700 in 12
// minutes), and the picture and the sound stuttered until U-APSD was withdrawn. EntryNav2 has the same head unit and
// chip.
//
// hostapd on the car is v2.1-devel. SET changes the setting in memory only: association responses follow it at once,
// but the beacon and the probe responses, which the chip sends from a template, go on offering U-APSD until hostapd
// rebuilds them. A phone asks for U-APSD from what they offer when it joins, and hostapd grants what it asked. RELOAD
// rebuilds them from the setting in memory, not from the file, but first disconnects every station. So RELOAD goes out
// only when the station list reads empty: an iPhone on CarPlay or a phone already on the car's Wi-Fi is never
// disconnected, and keeps U-APSD until it joins again. It also goes out only once per hostapd start: a phone coming
// back opens its Bluetooth link again about every 400 ms while it joins the Wi-Fi, and a RELOAD each time slowed its
// return from 2 s to 3 to 7 s on an F45 (EntryNav2-Android-Auto #164). wland can start hostapd again from its own file,
// so the setting itself is made before every Wi-Fi handshake.
//
// Pure: platform/evo_ap.cpp runs the hostapd_cli commands. The station list is only checked for being empty and is
// never logged, since it holds MAC addresses. Tested in tests/test_parsers.cpp.
#ifndef AA_ENTRYNAV_PLATFORM_AP_UAPSD_HPP
#define AA_ENTRYNAV_PLATFORM_AP_UAPSD_HPP

#include <string>
#include <vector>

namespace evo {

enum class UapsdOffer {
    kWithdrawn,      // off, and the beacon rebuilt: the next phone joins without U-APSD
    kAlreadyDone,    // off, and the beacon was rebuilt earlier for this hostapd start: no RELOAD again
    kBeaconLeft,     // off for new associations; a station is associated, or the list unreadable: no RELOAD
    kReloadRefused,  // off in hostapd, but RELOAD refused or unanswered: the beacon may still offer it
    kSetRefused,     // hostapd refused the setting or did not answer: left on
};

inline const char* describeUapsdOffer(UapsdOffer offer) {
    switch (offer) {
        case UapsdOffer::kWithdrawn:
            return "U-APSD off, beacon rebuilt: the phone joins without it";
        case UapsdOffer::kAlreadyDone:
            return "U-APSD off, beacon already rebuilt for this hostapd start";
        case UapsdOffer::kBeaconLeft:
            return "U-APSD off for new associations; a station is associated or unreadable, beacon left, nobody "
                   "disconnected";
        case UapsdOffer::kReloadRefused:
            return "U-APSD off in hostapd, RELOAD refused: the beacon may still offer it";
        case UapsdOffer::kSetRefused:
            break;
    }
    return "hostapd refused the setting: U-APSD left on";
}

namespace uapsd {
inline bool blank(const std::string& reply) {
    return reply.find_first_not_of(" \t\r\n") == std::string::npos;
}
// hostapd_cli prints hostapd's reply as it is: "OK" or "FAIL", then an end of line.
inline bool ok(const std::string& reply) {
    const size_t end = reply.find_last_not_of(" \t\r\n");
    return end != std::string::npos && reply.compare(0, end + 1, "OK") == 0;
}
}  // namespace uapsd

// Which hostapd start this is: its control socket, /var/run/hostapd/wlan0, is created anew each time hostapd starts.
// An unknown start is never the same as another.
struct HostapdStart {
    unsigned long long device, inode;
    bool known;
    HostapdStart() : device(0), inode(0), known(false) {}
    bool same(const HostapdStart& other) const {
        return known && other.known && device == other.device && inode == other.inode;
    }
};

// call(command, reply) runs one hostapd_cli command on wlan0, such as {"all_sta"}, and gives its reply; false when it
// could not run. At most three calls: the setting, the station list, RELOAD. beaconDone: the beacon was already rebuilt
// for this hostapd start, so the setting alone is made.
template <typename Call>
UapsdOffer withdrawUapsd(Call call, bool beaconDone) {
    std::string reply;
    std::vector<std::string> command;
    command.push_back("set");
    command.push_back("uapsd_advertisement_enabled");
    command.push_back("0");
    if (!call(command, reply) || !uapsd::ok(reply)) {
        return UapsdOffer::kSetRefused;
    }
    if (beaconDone) {
        return UapsdOffer::kAlreadyDone;
    }
    if (!call(std::vector<std::string>(1, "all_sta"), reply) || !uapsd::blank(reply)) {
        return UapsdOffer::kBeaconLeft;
    }
    command.assign(1, "raw");
    command.push_back("RELOAD");
    if (!call(command, reply) || !uapsd::ok(reply)) {
        return UapsdOffer::kReloadRefused;
    }
    return UapsdOffer::kWithdrawn;
}

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_AP_UAPSD_HPP
