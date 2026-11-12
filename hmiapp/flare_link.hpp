// Copyright (c) 2026 CallMeLewis
// What the Flare Drive page's QR code holds: how Flare Drive joins the car's Wi-Fi and finds the head unit on it.
//
//   flaredrive://car?v=1&s=<Wi-Fi name>&p=<Wi-Fi password>&h=<head unit's address>
//
// Values are percent-encoded (RFC 3986): everything but letters, digits and "-._~". Flare Drive ignores parameters it
// does not know, so later versions may add some; v changes only if one of these changes meaning. Never the root
// password. No SSH host key fingerprint: the head unit's keys come with BMW's system, the same in every car.
//
// The Wi-Fi name and password come from wland's ap.conf, read with the service's rules (platform/evo_ap_config.cpp):
// WPA2 with a passphrase only. Written without the C++ library, like the rest of the iDrive app.
//
// Pure: no BMW call, no file. tests/test_flare_link.cpp.
#ifndef AA_ENTRYNAV_HMIAPP_FLARE_LINK_HPP
#define AA_ENTRYNAV_HMIAPP_FLARE_LINK_HPP

#include <stddef.h>
#include <string.h>

namespace evo {
namespace app {

struct CarWifi {
    char ssid[33];      // up to 32 bytes
    char password[64];  // a WPA passphrase: 8 to 63 printable ASCII characters
};

// hostapd's configuration text (ap.conf) into out. False if it is not WPA2 with a passphrase, or a value is missing.
inline bool parseApConf(const char* text, CarWifi& out) {
    CarWifi w;
    memset(&w, 0, sizeof(w));
    bool ssid = false, password = false, badSecurity = false;
    size_t total = 0;
    for (const char* line = text; *line;) {
        const char* end = strchr(line, '\n');
        const size_t len = end ? static_cast<size_t>(end - line) : strlen(line);
        total += len + 1;
        if (total > 65536) {
            return false;
        }
        size_t n = len;
        if (n > 0 && line[n - 1] == '\r') {
            --n;
        }
        const char* eq = static_cast<const char*>(memchr(line, '=', n));
        if (n > 0 && line[0] != '#' && line[0] != ';' && eq) {
            const size_t nameLen = static_cast<size_t>(eq - line);
            const char* value = eq + 1;
            const size_t valueLen = n - nameLen - 1;
            struct Name {
                static bool is(const char* line, size_t len, const char* name) {
                    return strlen(name) == len && memcmp(line, name, len) == 0;
                }
                static bool value(const char* v, size_t len, const char* want) {
                    return strlen(want) == len && memcmp(v, want, len) == 0;
                }
            };
            if (Name::is(line, nameLen, "ssid")) {
                if (valueLen == 0 || valueLen > 32) {
                    return false;
                }
                memcpy(w.ssid, value, valueLen);
                w.ssid[valueLen] = '\0';
                ssid = true;
            } else if (Name::is(line, nameLen, "passphrase") || Name::is(line, nameLen, "wpa_passphrase")) {
                if (valueLen < 8 || valueLen > 63) {
                    return false;  // a raw 64-hex PSK is no passphrase
                }
                for (size_t i = 0; i < valueLen; ++i) {
                    if (static_cast<unsigned char>(value[i]) < 32 || static_cast<unsigned char>(value[i]) > 126) {
                        return false;
                    }
                }
                memcpy(w.password, value, valueLen);
                w.password[valueLen] = '\0';
                password = true;
            } else if (Name::is(line, nameLen, "wpa")) {
                badSecurity = badSecurity || !(Name::value(value, valueLen, "2") || Name::value(value, valueLen, "3"));
            } else if (Name::is(line, nameLen, "key_mgmt") || Name::is(line, nameLen, "wpa_key_mgmt")) {
                badSecurity = badSecurity || !Name::value(value, valueLen, "WPA-PSK");
            }
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
    if (!ssid || !password || badSecurity) {
        return false;
    }
    out = w;
    return true;
}

// The link into out (cap bytes with its terminating 0). Its length, or 0 if a value is missing or it does not fit: the
// page then says the car's Wi-Fi is off rather than show a code that cannot work.
inline size_t flareDriveLink(const char* ssid, const char* password, const char* host, char* out, size_t cap) {
    if (!ssid[0] || !password[0] || !host[0] || cap == 0) {
        return 0;
    }
    static const char kHex[] = "0123456789ABCDEF";
    size_t n = 0;
    bool fits = true;
    struct Out {
        static void put(char* out, size_t cap, size_t& n, bool& fits, char c) {
            if (n + 1 < cap) {
                out[n++] = c;
            } else {
                fits = false;
            }
        }
    };
    const char* const parts[3][2] = {{"flaredrive://car?v=1&s=", ssid}, {"&p=", password}, {"&h=", host}};
    for (int k = 0; k < 3; ++k) {
        for (const char* p = parts[k][0]; *p; ++p) {
            Out::put(out, cap, n, fits, *p);
        }
        for (const char* p = parts[k][1]; *p; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                               c == '.' || c == '_' || c == '~';
            if (plain) {
                Out::put(out, cap, n, fits, static_cast<char>(c));
            } else {
                Out::put(out, cap, n, fits, '%');
                Out::put(out, cap, n, fits, kHex[c >> 4]);
                Out::put(out, cap, n, fits, kHex[c & 15]);
            }
        }
    }
    out[n] = '\0';
    if (!fits) {
        out[0] = '\0';
        return 0;
    }
    return n;
}

}  // namespace app
}  // namespace evo

#endif
