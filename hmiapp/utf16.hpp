// Copyright (c) 2026 CallMeLewis
// UTF-8 text from the phone into the UTF-16 units iDrive's strings hold (THmiString<unsigned short>): pure.
//
// A malformed sequence becomes U+FFFD and decoding goes on; a character beyond U+FFFF becomes its surrogate pair. At
// most max units are written: a pair is never cut in half. tests/test_utf16.cpp.
#ifndef AA_ENTRYNAV_HMIAPP_UTF16_HPP
#define AA_ENTRYNAV_HMIAPP_UTF16_HPP

#include <stddef.h>
#include <stdint.h>

namespace evo {
namespace app {

// The number of units written to out.
inline size_t utf8ToUtf16(const char* text, size_t size, uint16_t* out, size_t max) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(text);
    size_t i = 0, n = 0;
    while (i < size) {
        uint32_t c = s[i];
        size_t extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 4;
        uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1f) : extra == 2 ? (c & 0x0f) : (c & 0x07);
        bool ok = extra < 4;  // 4: a continuation byte or 0xf8 and above, never a lead byte
        for (size_t k = 1; ok && k <= extra; ++k) {
            if (i + k >= size || (s[i + k] & 0xc0) != 0x80) {
                ok = false;
            } else {
                cp = (cp << 6) | (s[i + k] & 0x3f);
            }
        }
        static const uint32_t kMin[] = {0, 0x80, 0x800, 0x10000};
        if (ok && (cp < kMin[extra] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))) {
            ok = false;  // overlong, out of range or a lone surrogate
        }
        if (!ok) {
            cp = 0xfffd;
            extra = 0;  // skip the bad lead byte alone; what follows is decoded afresh
        }
        if (cp >= 0x10000) {
            if (n + 2 > max) {
                break;
            }
            cp -= 0x10000;
            out[n++] = static_cast<uint16_t>(0xd800 + (cp >> 10));
            out[n++] = static_cast<uint16_t>(0xdc00 + (cp & 0x3ff));
        } else {
            if (n + 1 > max) {
                break;
            }
            out[n++] = static_cast<uint16_t>(cp);
        }
        i += extra + 1;
    }
    return n;
}

}  // namespace app
}  // namespace evo

#endif  // AA_ENTRYNAV_HMIAPP_UTF16_HPP
