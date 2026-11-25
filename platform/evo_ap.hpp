// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's Wi-Fi access point (SSID, passphrase, BSSID, IP) that the phone is told to join.
#ifndef AA_ENTRYNAV_PLATFORM_EVO_AP_HPP
#define AA_ENTRYNAV_PLATFORM_EVO_AP_HPP
#include <string>

#include "ap_uapsd.hpp"
#include "wireless_setup.hpp"
namespace evo {
// Pure parser for hostapd get_config / ap.conf. Does not log credentials.
bool parseHostapdConfig(const std::string& text, aa::ApInfo& out);
// Read-only hostapd query (bounded child), then ap.conf; IP/MAC from wlan0 ioctls.
bool loadAccessPoint(aa::ApInfo& out, std::string& source);
// No U-APSD offered to the next phone that joins (ap_uapsd.hpp), through hostapd_cli as bounded children; the beacon
// rebuilt at most once per hostapd start. Not logged here. The wireless loop's thread only.
UapsdOffer withdrawAccessPointUapsd();
}  // namespace evo
#endif
