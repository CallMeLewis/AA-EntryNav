// Copyright (c) 2026 CallMeLewis
// hmiapp/qr_png.cpp: the PNG file read back by an independent reader here: signature, chunks and their CRCs, IHDR, the
// zlib stream and its stored blocks, Adler-32, then every pixel against the code's modules.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hmiapp/qr_code.hpp"
#include "../hmiapp/qr_png.hpp"

namespace {

using evo::app::QrCode;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

uint32_t get32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
}

uint32_t bitwiseCrc(const unsigned char* d, size_t n) {  // slow, table-free: not the writer's code
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

// Reads the file back; the pixels into rgb (side * side * 3). False with a message on the first fault.
bool readPng(const unsigned char* png, size_t size, int side, unsigned char* rgb) {
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < 8 || memcmp(png, kSig, 8) != 0) {
        printf("  bad signature\n");
        return false;
    }
    size_t pos = 8;
    unsigned char* zdata = static_cast<unsigned char*>(malloc(size));
    size_t zlen = 0;
    bool sawIhdr = false, sawEnd = false;
    while (pos + 12 <= size && !sawEnd) {
        const uint32_t len = get32(png + pos);
        const unsigned char* type = png + pos + 4;
        if (pos + 12 + len > size || bitwiseCrc(type, 4 + len) != get32(png + pos + 8 + len)) {
            printf("  bad chunk at %u\n", static_cast<unsigned>(pos));
            free(zdata);
            return false;
        }
        const unsigned char* data = png + pos + 8;
        if (!memcmp(type, "IHDR", 4)) {
            sawIhdr = len == 13 && get32(data) == static_cast<uint32_t>(side) &&
                      get32(data + 4) == static_cast<uint32_t>(side) && data[8] == 8 && data[9] == 2 && data[10] == 0 &&
                      data[11] == 0 && data[12] == 0;
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(zdata + zlen, data, len);
            zlen += len;
        } else if (!memcmp(type, "IEND", 4)) {
            sawEnd = len == 0 && pos + 12 == size;
        }
        pos += 12 + len;
    }
    bool ok = sawIhdr && sawEnd && zlen >= 6 && zdata[0] == 0x78 && (zdata[0] * 256 + zdata[1]) % 31 == 0;
    const size_t rowLen = 1 + 3 * static_cast<size_t>(side);
    unsigned char* raw = static_cast<unsigned char*>(malloc(rowLen * side + 1));
    size_t rawLen = 0, z = 2;
    bool last = false;
    while (ok && !last) {
        if (z + 5 > zlen) {
            ok = false;
            break;
        }
        last = (zdata[z] & 1) != 0;
        ok = (zdata[z] & 6) == 0;  // stored
        const unsigned blockLen = zdata[z + 1] | zdata[z + 2] << 8;
        const unsigned nlen = zdata[z + 3] | zdata[z + 4] << 8;
        ok = ok && (blockLen ^ 0xFFFFu) == nlen && z + 5 + blockLen <= zlen && rawLen + blockLen <= rowLen * side;
        if (ok) {
            memcpy(raw + rawLen, zdata + z + 5, blockLen);
            rawLen += blockLen;
            z += 5 + blockLen;
        }
    }
    ok = ok && rawLen == rowLen * side && z + 4 == zlen;
    if (ok) {
        uint32_t a = 1, b = 0;
        for (size_t i = 0; i < rawLen; ++i) {
            a = (a + raw[i]) % 65521;
            b = (b + a) % 65521;
        }
        ok = get32(zdata + z) == (b << 16 | a);
    }
    for (int y = 0; ok && y < side; ++y) {
        ok = raw[y * rowLen] == 0;  // filter None
        memcpy(rgb + 3 * side * y, raw + y * rowLen + 1, 3 * static_cast<size_t>(side));
    }
    if (!ok) {
        printf("  bad zlib stream or rows\n");
    }
    free(raw);
    free(zdata);
    return ok;
}

void picture(const char* text, int side) {
    static QrCode q;
    CHECK(q.encode(reinterpret_cast<const unsigned char*>(text), strlen(text)));
    const size_t size = evo::app::qrPngSize(side);
    unsigned char* png = static_cast<unsigned char*>(malloc(size));
    CHECK(evo::app::qrPng(q, side, png, size));
    unsigned char* rgb = static_cast<unsigned char*>(malloc(3 * static_cast<size_t>(side) * side));
    CHECK(readPng(png, size, side, rgb));
    const int modules = q.size() + 8;
    const int scale = side / modules;
    const int left = (side - modules * scale) / 2;  // where the quiet zone starts
    int wrong = 0;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const int mx = (x - left) / scale - 4, my = (y - left) / scale - 4;
            const bool inCode = x >= left + 4 * scale && y >= left + 4 * scale && mx < q.size() && my < q.size();
            const unsigned char want = inCode && q.dark(mx, my) ? 0 : 255;
            const unsigned char* px = rgb + 3 * (static_cast<size_t>(side) * y + x);
            if (px[0] != want || px[1] != want || px[2] != want) {
                ++wrong;
            }
        }
    }
    if (wrong) {
        printf("FAIL: %d px wrong for a %d px picture of version %d\n", wrong, side, q.version());
        ++g_failures;
    }
    free(rgb);
    free(png);
}

void pictures() {
    picture("HELLO", 360);  // version 1: 29 modules with the quiet zone, 12 px each, 6 px margin
    picture("flaredrive://car?v=1&s=BMW12345&p=abcdefgh&h=172.16.222.49", 360);
    char big[400];
    memset(big, 'q', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    picture(big, 360);     // version 15: 85 modules, 4 px each
    picture("HELLO", 29);  // one pixel per module, no margin
}

void sizes() {
    CHECK(evo::app::qrPngSize(0) == 0);
    CHECK(evo::app::qrPngSize(360) < 1024000);  // what the iDrive accepts
    // 360 rows of 1081 bytes: 389,160 raw bytes in 6 stored blocks.
    CHECK(evo::app::qrPngSize(360) == 8 + 25 + 12 + 2 + 389160 + 5 * 6 + 4 + 12);
}

void refusals() {
    static QrCode q;
    unsigned char buf[16];
    CHECK(!evo::app::qrPng(q, 100, buf, sizeof(buf)));  // nothing encoded yet
    CHECK(q.encode(reinterpret_cast<const unsigned char*>("HELLO"), 5));
    const size_t size = evo::app::qrPngSize(28);
    unsigned char* png = static_cast<unsigned char*>(malloc(size));
    CHECK(!evo::app::qrPng(q, 28, png, size));  // 29 modules do not fit 28 px
    free(png);
    CHECK(!evo::app::qrPng(q, 360, buf, sizeof(buf)));  // no room
}

}  // namespace

int main() {
    pictures();
    sizes();
    refusals();
    if (g_failures) {
        printf("test_qr_png: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_qr_png: OK\n");
    return 0;
}
