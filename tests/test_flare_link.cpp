// Copyright (c) 2026 CallMeLewis
// hmiapp/flare_link.hpp: reading the car's Wi-Fi from ap.conf, the link in the Flare Drive page's QR code, and that the
// longest link still fits a QR code.
#include <stdio.h>
#include <string.h>

#include "../hmiapp/flare_link.hpp"
#include "../hmiapp/qr_code.hpp"

namespace {

using evo::app::CarWifi;
using evo::app::flareDriveLink;
using evo::app::parseApConf;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

bool linkIs(const char* ssid, const char* password, const char* host, const char* want) {
    char out[512];
    const size_t n = flareDriveLink(ssid, password, host, out, sizeof(out));
    if (n != strlen(want) || strcmp(out, want) != 0) {
        printf("  got '%s'\n", out);
        return false;
    }
    return true;
}

void plainValues() {
    CHECK(
        linkIs("BMW12345", "abcdefgh", "172.16.222.49", "flaredrive://car?v=1&s=BMW12345&p=abcdefgh&h=172.16.222.49"));
}

void encodedValues() {
    // Spaces, the link's own separators, '%', '+' and bytes above 127 are encoded; "-._~" are not.
    CHECK(linkIs("My BMW&Co", "p=a%b+c#d", "10.0.0.1",
                 "flaredrive://car?v=1&s=My%20BMW%26Co&p=p%3Da%25b%2Bc%23d&h=10.0.0.1"));
    CHECK(linkIs("a-b.c_d~e", "\xC3\xA9t\xC3\xA9!*'()", "h",
                 "flaredrive://car?v=1&s=a-b.c_d~e&p=%C3%A9t%C3%A9%21%2A%27%28%29&h=h"));
}

void missingValuesOrNoRoom() {
    char out[512];
    CHECK(flareDriveLink("", "abcdefgh", "1.2.3.4", out, sizeof(out)) == 0);
    CHECK(flareDriveLink("BMW", "", "1.2.3.4", out, sizeof(out)) == 0);
    CHECK(flareDriveLink("BMW", "abcdefgh", "", out, sizeof(out)) == 0);
    char small[30];
    CHECK(flareDriveLink("BMW12345", "abcdefgh", "172.16.222.49", small, sizeof(small)) == 0);
    CHECK(small[0] == '\0');  // never a cut link
    const char* exact = "flaredrive://car?v=1&s=B&p=abcdefgh&h=h";
    char fit[64];
    CHECK(flareDriveLink("B", "abcdefgh", "h", fit, strlen(exact) + 1) == strlen(exact));
    CHECK(flareDriveLink("B", "abcdefgh", "h", fit, strlen(exact)) == 0);
}

void longestFits() {
    // Wi-Fi limits: a 32-byte name, a 63-character password; every byte encoded.
    char ssid[33], password[64], out[512];
    memset(ssid, ' ', 32);
    ssid[32] = '\0';
    memset(password, '%', 63);
    password[63] = '\0';
    const size_t n = flareDriveLink(ssid, password, "255.255.255.255", out, sizeof(out));
    CHECK(n == 23 + 96 + 3 + 189 + 3 + 15);
    static evo::app::QrCode q;
    CHECK(q.encode(reinterpret_cast<const unsigned char*>(out), n));
}

void apConf() {
    CarWifi w;
    CHECK(parseApConf("interface=wlan0\nssid=BMW 12345\nwpa=2\nwpa_key_mgmt=WPA-PSK\nwpa_passphrase=abcdefgh12\n", w));
    CHECK(strcmp(w.ssid, "BMW 12345") == 0);
    CHECK(strcmp(w.password, "abcdefgh12") == 0);
    CHECK(parseApConf("# comment\r\nssid=X\r\npassphrase=12345678\r\n", w));  // CR LF, the other key name
    CHECK(strcmp(w.ssid, "X") == 0 && strcmp(w.password, "12345678") == 0);
    CHECK(parseApConf("ssid=X\npassphrase=12345678", w));         // no final line end
    CHECK(parseApConf("ssid=X\nwpa=3\npassphrase=a=b=c=d=", w));  // '=' inside the value
    CHECK(strcmp(w.password, "a=b=c=d=") == 0);
}

void apConfRefused() {
    CarWifi w;
    strcpy(w.ssid, "kept");
    CHECK(!parseApConf("", w));
    CHECK(!parseApConf("ssid=X\n", w));                                  // no password
    CHECK(!parseApConf("wpa_passphrase=12345678\n", w));                 // no name
    CHECK(!parseApConf("ssid=\nwpa_passphrase=12345678\n", w));          // empty name
    CHECK(!parseApConf("ssid=X\nwpa_passphrase=1234567\n", w));          // 7 characters
    CHECK(!parseApConf("ssid=X\nwpa_passphrase=1234567\x01\n", w));      // a control character
    CHECK(!parseApConf("ssid=X\nwpa=1\nwpa_passphrase=12345678\n", w));  // WPA1
    CHECK(!parseApConf("ssid=X\nkey_mgmt=SAE\nwpa_passphrase=12345678\n", w));
    char longName[64];
    strcpy(longName, "ssid=");
    memset(longName + 5, 'n', 33);
    strcpy(longName + 38, "\npassphrase=12345678\n");
    CHECK(!parseApConf(longName, w));
    char hexKey[100];
    strcpy(hexKey, "ssid=X\nwpa_passphrase=");
    memset(hexKey + 22, 'a', 64);
    hexKey[86] = '\0';
    CHECK(!parseApConf(hexKey, w));      // a raw 64-hex PSK
    CHECK(strcmp(w.ssid, "kept") == 0);  // a refusal leaves out alone
}

}  // namespace

int main() {
    plainValues();
    encodedValues();
    missingValuesOrNoRoom();
    longestFits();
    apConf();
    apConfRefused();
    if (g_failures) {
        printf("test_flare_link: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_flare_link: OK\n");
    return 0;
}
