// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Alps SPP UUID and payload encoding, the hostapd configuration parser and the access point's U-APSD steps, on made-up
// values.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "alps_spp.hpp"
#include "ap_uapsd.hpp"
#include "evo_ap.hpp"
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)

namespace {

// hostapd as hostapd_cli shows it: one reply per command, in order, "!" for a command that could not run. The commands
// sent are kept.
std::vector<std::string> g_replies, g_sent;

bool fakeHostapd(const std::vector<std::string>& command, std::string& reply) {
    std::string line;
    for (size_t i = 0; i < command.size(); ++i) {
        line += (i ? " " : "") + command[i];
    }
    g_sent.push_back(line);
    const std::string next = g_sent.size() <= g_replies.size() ? g_replies[g_sent.size() - 1] : "!";
    reply = next == "!" ? "" : next;
    return next != "!";
}

evo::UapsdOffer withdraw(const std::vector<std::string>& answers, bool beaconDone = false) {
    g_replies = answers;
    g_sent.clear();
    return evo::withdrawUapsd(fakeHostapd, beaconDone);
}

std::vector<std::string> replies(const char* a, const char* b = 0, const char* c = 0) {
    std::vector<std::string> out(1, a);
    if (b) {
        out.push_back(b);
    }
    if (c) {
        out.push_back(c);
    }
    return out;
}

void uapsdCases() {
    // No station: the setting, then RELOAD to rebuild the beacon and the chip's probe response.
    CHECK(withdraw(replies("OK\n", "", "OK\n")) == evo::UapsdOffer::kWithdrawn);
    CHECK(g_sent.size() == 3 && g_sent[0] == "set uapsd_advertisement_enabled 0" && g_sent[1] == "all_sta" &&
          g_sent[2] == "raw RELOAD");
    CHECK(withdraw(replies("OK", " \n", "OK\r\n")) == evo::UapsdOffer::kWithdrawn);
    // A station on the access point (its MIB lines): RELOAD would disconnect it, so it is not sent.
    CHECK(withdraw(replies("OK\n", "02:00:00:00:00:01\nflags=[AUTH][ASSOC]\n")) == evo::UapsdOffer::kBeaconLeft);
    CHECK(g_sent.size() == 2);
    // A station list that cannot be read is no proof of an empty one.
    CHECK(withdraw(replies("OK\n", "!")) == evo::UapsdOffer::kBeaconLeft && g_sent.size() == 2);
    CHECK(withdraw(replies("OK\n", "'STA-FIRST' command failed.\n")) == evo::UapsdOffer::kBeaconLeft);
    // The setting refused or not answered: nothing else is sent.
    CHECK(withdraw(replies("FAIL\n")) == evo::UapsdOffer::kSetRefused && g_sent.size() == 1);
    CHECK(withdraw(replies("!")) == evo::UapsdOffer::kSetRefused && g_sent.size() == 1);
    CHECK(withdraw(replies("OKAY\n")) == evo::UapsdOffer::kSetRefused);
    CHECK(withdraw(replies("OK\n", "", "FAIL\n")) == evo::UapsdOffer::kReloadRefused);
    CHECK(withdraw(replies("OK\n", "", "!")) == evo::UapsdOffer::kReloadRefused);
    // The beacon already rebuilt for this hostapd start: the setting again, never a second RELOAD.
    CHECK(withdraw(replies("OK\n", "", "OK\n"), true) == evo::UapsdOffer::kAlreadyDone);
    CHECK(g_sent.size() == 1 && g_sent[0] == "set uapsd_advertisement_enabled 0");
    CHECK(withdraw(replies("FAIL\n"), true) == evo::UapsdOffer::kSetRefused);
    // Which hostapd start: its control socket's identity. An unknown start is never the same one.
    evo::HostapdStart a, b, unknown;
    a.device = b.device = 1;
    a.inode = b.inode = 42;
    a.known = b.known = true;
    CHECK(a.same(b) && !a.same(unknown) && !unknown.same(unknown));
    b.inode = 43;  // hostapd started again: a new socket
    CHECK(!a.same(b));
    CHECK(strstr(evo::describeUapsdOffer(evo::UapsdOffer::kSetRefused), "left on"));
}

}  // namespace

int main() {
    uint8_t uuid[16];
    const uint8_t expected[] = {0x4d, 0xe1, 0x7a, 0x00, 0x52, 0xcb, 0x11, 0xe6,
                                0xbd, 0xf4, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66};
    CHECK(evo::parseUuid128(evo::kAaUuid, uuid));
    CHECK(!memcmp(uuid, expected, 16));
    CHECK(evo::parseUuid128("4DE17A00-52CB-11E6-BDF4-0800200C9A66", uuid));
    CHECK(!memcmp(uuid, expected, 16));
    const char* bad[] = {"", "4de17a00-52cb-11e6-bdf4-0800200c9a6", "4de17a00-52cb-11e6-bdf4-0800200c9a660",
                         "4de17a00_52cb-11e6-bdf4-0800200c9a66", "4de17a00-52cb-11e6-bdf4-0800200c9a6g"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(uuid, 0xaa, sizeof(uuid));
        CHECK(!evo::parseUuid128(bad[i], uuid));
        CHECK(uuid[0] == 0xaa);
    }
    CHECK(!evo::parseUuid128(0, uuid));
    CHECK(!evo::parseUuid128(evo::kAaUuid, 0));
    uint8_t local[16];
    memset(local, 0x42, sizeof(local));
    std::vector<uint8_t> payload = evo::buildSetUuidPayload(expected, local);
    CHECK(payload.size() == 34 && payload[0] == 3 && payload[17] == 3);
    CHECK(!memcmp(&payload[1], expected, 16) && !memcmp(&payload[18], local, 16));
    CHECK(evo::buildSetUuidPayload(0, local).empty());
    aa::ApInfo ap;
    ap.ssid = "unchanged";
    CHECK(
        evo::parseHostapdConfig("Selected interface 'wlan0'\nssid=BMW test\npassphrase=demo#= pass "
                                "\nkey_mgmt=WPA-PSK\nbssid=12:34:56:78:9a:bc\n",
                                ap));
    CHECK(ap.ssid == "BMW test" && ap.key == "demo#= pass " && ap.security == 8);
    CHECK(evo::parseHostapdConfig("# comment\r\nssid=BMW\r\nwpa_passphrase=demo1234\r\nwpa=2\r\n", ap));
    CHECK(ap.key == "demo1234" && ap.bssid.empty());
    CHECK(!evo::parseHostapdConfig("ssid=bad\nwpa_passphrase=short\n", ap));
    CHECK(ap.ssid == "BMW");
    CHECK(!evo::parseHostapdConfig("ssid=bad\npsk=" + std::string(64, 'a') + "\n", ap));
    CHECK(!evo::parseHostapdConfig("ssid=bad\npassphrase=demo1234\nkey_mgmt=WPA-EAP\n", ap));
    CHECK(!evo::parseHostapdConfig("ssid=bad\npassphrase=demo1234\nwpa=0\n", ap));
    CHECK(!evo::parseHostapdConfig("ssid=" + std::string(33, 's') + "\npassphrase=demo1234\n", ap));
    CHECK(!evo::parseHostapdConfig("ssid=ok\npassphrase=" + std::string(64, 'k') + "\n", ap));
    CHECK(!evo::parseHostapdConfig("ssid=ok\npassphrase=demo1234" + std::string(1, '\0'), ap));
    CHECK(!evo::parseHostapdConfig(std::string(65537, 'x'), ap));
    uapsdCases();
    puts(
        "PASS Alps UUID/payload and hostapd configuration (no real secret); access point U-APSD: RELOAD only with no "
        "station and once per hostapd start");
    return 0;
}
