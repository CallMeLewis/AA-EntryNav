// Copyright (c) 2026 CallMeLewis
// A QR code as an uncompressed PNG file: see qr_png.hpp. PNG: ISO/IEC 15948; zlib and deflate: RFC 1950 and 1951.
#include "qr_png.hpp"

#include <stdint.h>
#include <string.h>

namespace evo {
namespace app {

namespace {

const size_t kStoredMax = 65535;  // bytes in one stored deflate block

size_t rowBytes(int side) {
    return 1 + 3 * static_cast<size_t>(side);
}  // filter byte, then RGB

size_t rawBytes(int side) {
    return static_cast<size_t>(side) * rowBytes(side);
}

size_t storedBlocks(size_t raw) {
    return raw == 0 ? 1 : (raw + kStoredMax - 1) / kStoredMax;
}

uint32_t crc32(const unsigned char* data, size_t size, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void put32(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v >> 24);
    p[1] = static_cast<unsigned char>(v >> 16);
    p[2] = static_cast<unsigned char>(v >> 8);
    p[3] = static_cast<unsigned char>(v);
}

// A chunk whose data is already at p + 8: writes its length, type and CRC around it. Returns the bytes used.
size_t closeChunk(unsigned char* p, const char* type, size_t length) {
    put32(p, static_cast<uint32_t>(length));
    memcpy(p + 4, type, 4);
    put32(p + 8 + length, crc32(p + 4, 4 + length));
    return 12 + length;
}

}  // namespace

size_t qrPngSize(int side) {
    if (side < 1) {
        return 0;
    }
    const size_t raw = rawBytes(side);
    const size_t idat = 2 + raw + 5 * storedBlocks(raw) + 4;  // zlib header, stored blocks, Adler-32
    return 8 + (12 + 13) + (12 + idat) + 12;
}

bool qrPng(const QrCode& code, int side, unsigned char* out, size_t cap) {
    const int modules = code.size() + 2 * QrCode::kQuietZone;
    if (side < 1 || code.size() == 0 || cap < qrPngSize(side) || side < modules) {
        return false;
    }
    const int scale = side / modules;
    const int origin = (side - modules * scale) / 2 + QrCode::kQuietZone * scale;  // the code's top left pixel
    static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    unsigned char* p = out;
    memcpy(p, kSignature, 8);
    p += 8;

    unsigned char* ihdr = p + 8;
    put32(ihdr, static_cast<uint32_t>(side));
    put32(ihdr + 4, static_cast<uint32_t>(side));
    ihdr[8] = 8;   // bits per sample
    ihdr[9] = 2;   // RGB
    ihdr[10] = 0;  // deflate
    ihdr[11] = 0;  // adaptive filtering (every row uses filter 0, None)
    ihdr[12] = 0;  // no interlace
    p += closeChunk(p, "IHDR", 13);

    // IDAT: zlib header (deflate, 32 KiB window, no dictionary, fastest), the rows in stored blocks, Adler-32.
    unsigned char* idat = p + 8;
    unsigned char* z = idat;
    *z++ = 0x78;
    *z++ = 0x01;
    const size_t raw = rawBytes(side), rowLen = rowBytes(side);
    uint32_t adlerA = 1, adlerB = 0;
    size_t written = 0;  // raw bytes so far
    size_t blockLeft = 0;
    for (int y = 0; y < side; ++y) {
        for (size_t i = 0; i < rowLen; ++i) {
            if (blockLeft == 0) {
                const size_t len = raw - written < kStoredMax ? raw - written : kStoredMax;
                *z++ = written + len == raw ? 1 : 0;  // BFINAL on the last block, BTYPE 00
                *z++ = static_cast<unsigned char>(len);
                *z++ = static_cast<unsigned char>(len >> 8);
                *z++ = static_cast<unsigned char>(~len);
                *z++ = static_cast<unsigned char>(~len >> 8);
                blockLeft = len;
            }
            unsigned char byte = 0;  // the filter byte
            if (i > 0) {
                const int x = static_cast<int>((i - 1) / 3);
                const int mx = x - origin, my = y - origin;
                const bool inside = mx >= 0 && my >= 0 && mx < code.size() * scale && my < code.size() * scale;
                byte = inside && code.dark(mx / scale, my / scale) ? 0x00 : 0xFF;
            }
            *z++ = byte;
            adlerA = (adlerA + byte) % 65521;
            adlerB = (adlerB + adlerA) % 65521;
            ++written;
            --blockLeft;
        }
    }
    put32(z, adlerB << 16 | adlerA);
    z += 4;
    p += closeChunk(p, "IDAT", static_cast<size_t>(z - idat));
    p += closeChunk(p, "IEND", 0);
    return static_cast<size_t>(p - out) == qrPngSize(side);
}

}  // namespace app
}  // namespace evo
