// Copyright (c) 2026 CallMeLewis
// QR code encoder: see qr_code.hpp. Layout as in ISO/IEC 18004:2015, sections 7.4 to 7.9.
#include "qr_code.hpp"

#include <string.h>

namespace evo {
namespace app {

namespace {

// Level M, versions 1 to 15 (ISO/IEC 18004 table 9): error correction codewords per block, and blocks.
const int kEccPerBlock[QrCode::kMaxVersion + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24};
const int kBlocks[QrCode::kMaxVersion + 1] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10};
const int kFormatLevelM = 0;       // level M's two bits in the format information
const size_t kMaxCodewords = 655;  // version 15

// Modules left for data and error correction once the function patterns are drawn.
int rawDataModules(int version) {
    int n = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int align = version / 7 + 2;
        n -= (25 * align - 10) * align - 55;
        if (version >= 7) {
            n -= 36;  // the two version information blocks
        }
    }
    return n;
}

int dataCodewords(int version) {
    return rawDataModules(version) / 8 - kEccPerBlock[version] * kBlocks[version];
}

int countBits(int version) {
    return version <= 9 ? 8 : 16;
}

// Centres of the alignment patterns along one axis, 0 of them for version 1.
int alignmentPositions(int version, int out[7]) {
    if (version == 1) {
        return 0;
    }
    const int count = version / 7 + 2;
    const int size = 17 + 4 * version;
    const int step = (version * 8 + count * 3 + 5) / (count * 4 - 4) * 2;
    out[0] = 6;
    for (int i = count - 1, pos = size - 7; i >= 1; --i, pos -= step) {
        out[i] = pos;
    }
    return count;
}

// GF(2^8) with the polynomial x^8 + x^4 + x^3 + x^2 + 1.
uint8_t gfMultiply(uint8_t x, uint8_t y) {
    unsigned z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1u) * x;
    }
    return static_cast<uint8_t>(z);
}

// The Reed-Solomon generator's coefficients, highest first without the leading 1.
void rsDivisor(int degree, uint8_t* out) {
    memset(out, 0, static_cast<size_t>(degree));
    out[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            out[j] = gfMultiply(out[j], root);
            if (j + 1 < degree) {
                out[j] ^= out[j + 1];
            }
        }
        root = gfMultiply(root, 0x02);
    }
}

void rsRemainder(const uint8_t* data, int size, const uint8_t* divisor, int degree, uint8_t* out) {
    memset(out, 0, static_cast<size_t>(degree));
    for (int k = 0; k < size; ++k) {
        const uint8_t factor = data[k] ^ out[0];
        memmove(out, out + 1, static_cast<size_t>(degree - 1));
        out[degree - 1] = 0;
        for (int i = 0; i < degree; ++i) {
            out[i] ^= gfMultiply(divisor[i], factor);
        }
    }
}

class BitWriter {
public:
    BitWriter(uint8_t* out, size_t cap) : out_(out), cap_(cap), bits_(0) { memset(out, 0, cap); }
    void put(unsigned value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            if (bits_ / 8 < cap_ && ((value >> i) & 1u)) {
                out_[bits_ / 8] = static_cast<uint8_t>(out_[bits_ / 8] | (0x80 >> (bits_ % 8)));
            }
            ++bits_;
        }
    }
    size_t bits() const { return bits_; }

private:
    uint8_t* out_;
    size_t cap_;
    size_t bits_;
};

bool maskedAt(int mask, int x, int y) {
    switch (mask) {
        case 0:
            return (x + y) % 2 == 0;
        case 1:
            return y % 2 == 0;
        case 2:
            return x % 3 == 0;
        case 3:
            return (x + y) % 3 == 0;
        case 4:
            return (x / 3 + y / 2) % 2 == 0;
        case 5:
            return x * y % 2 + x * y % 3 == 0;
        case 6:
            return (x * y % 2 + x * y % 3) % 2 == 0;
        default:
            return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

}  // namespace

QrCode::QrCode() : version_(0), mask_(0), size_(0) {
    memset(modules_, 0, sizeof(modules_));
    memset(function_, 0, sizeof(function_));
}

size_t QrCode::capacity(int version) {
    if (version < 1 || version > kMaxVersion) {
        return 0;
    }
    return static_cast<size_t>((dataCodewords(version) * 8 - 4 - countBits(version)) / 8);
}

void QrCode::reset(int version) {
    version_ = version;
    size_ = 17 + 4 * version;
    memset(modules_, 0, sizeof(modules_));
    memset(function_, 0, sizeof(function_));
}

bool QrCode::encode(const uint8_t* data, size_t size, int mask) {
    int version = 1;
    while (version <= kMaxVersion && capacity(version) < size) {
        ++version;
    }
    if (version > kMaxVersion || mask < -1 || mask > 7) {
        size_ = 0;
        return false;
    }
    reset(version);

    // Data codewords: byte mode, the count, the bytes, the terminator, then the pad bytes.
    const int dataLen = dataCodewords(version);
    uint8_t codewords[kMaxCodewords];
    BitWriter bits(codewords, static_cast<size_t>(dataLen));
    bits.put(0x4, 4);
    bits.put(static_cast<unsigned>(size), countBits(version));
    for (size_t i = 0; i < size; ++i) {
        bits.put(data[i], 8);
    }
    const size_t capBits = static_cast<size_t>(dataLen) * 8;
    size_t terminator = capBits - bits.bits();
    bits.put(0, static_cast<int>(terminator < 4 ? terminator : 4));
    bits.put(0, static_cast<int>((8 - bits.bits() % 8) % 8));
    for (uint8_t pad = 0xEC; bits.bits() < capBits; pad ^= 0xEC ^ 0x11) {
        bits.put(pad, 8);
    }

    // Split into blocks, add each block's error correction, interleave (section 7.6).
    const int blocks = kBlocks[version], ecc = kEccPerBlock[version];
    const int raw = rawDataModules(version) / 8;
    const int shortBlocks = blocks - raw % blocks;
    const int shortLen = raw / blocks;  // data + error correction of a short block
    uint8_t divisor[30];
    rsDivisor(ecc, divisor);
    uint8_t blockEcc[10][30];
    int blockStart[10], blockData[10];
    for (int b = 0, start = 0; b < blocks; ++b) {
        blockStart[b] = start;
        blockData[b] = shortLen - ecc + (b < shortBlocks ? 0 : 1);
        rsRemainder(codewords + start, blockData[b], divisor, ecc, blockEcc[b]);
        start += blockData[b];
    }
    uint8_t all[kMaxCodewords];
    size_t n = 0;
    for (int i = 0; i < shortLen - ecc + 1; ++i) {
        for (int b = 0; b < blocks; ++b) {
            if (i < blockData[b]) {
                all[n++] = codewords[blockStart[b] + i];
            }
        }
    }
    for (int i = 0; i < ecc; ++i) {
        for (int b = 0; b < blocks; ++b) {
            all[n++] = blockEcc[b][i];
        }
    }

    drawFunctionPatterns();
    drawCodewords(all, n);
    if (mask < 0) {
        long best = -1;
        for (int m = 0; m < 8; ++m) {
            applyMask(m);
            drawFormat(m);
            const long p = penalty();
            if (best < 0 || p < best) {
                best = p;
                mask = m;
            }
            applyMask(m);  // XOR again: back to unmasked
        }
    }
    mask_ = mask;
    applyMask(mask);
    drawFormat(mask);
    return true;
}

void QrCode::drawFunctionPatterns() {
    for (int i = 0; i < size_; ++i) {
        setFunction(6, i, i % 2 == 0);
        setFunction(i, 6, i % 2 == 0);
    }
    drawFinder(3, 3);
    drawFinder(size_ - 4, 3);
    drawFinder(3, size_ - 4);
    int pos[7];
    const int count = alignmentPositions(version_, pos);
    for (int i = 0; i < count; ++i) {
        for (int j = 0; j < count; ++j) {
            const bool corner = (i == 0 && j == 0) || (i == 0 && j == count - 1) || (i == count - 1 && j == 0);
            if (!corner) {
                drawAlignment(pos[i], pos[j]);
            }
        }
    }
    drawFormat(0);  // reserves the modules; drawn for real once the mask is known
    drawVersion();
}

void QrCode::drawFinder(int x, int y) {
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            const int dist = ax > ay ? ax : ay;
            const int xx = x + dx, yy = y + dy;
            if (xx >= 0 && xx < size_ && yy >= 0 && yy < size_) {
                setFunction(xx, yy, dist != 2 && dist != 4);
            }
        }
    }
}

void QrCode::drawAlignment(int x, int y) {
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            setFunction(x + dx, y + dy, (ax > ay ? ax : ay) != 1);
        }
    }
}

void QrCode::drawFormat(int mask) {
    const unsigned data = static_cast<unsigned>(kFormatLevelM << 3 | mask);
    unsigned rem = data;
    for (int i = 0; i < 10; ++i) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }
    const unsigned bits = (data << 10 | rem) ^ 0x5412;
    struct Bit {
        static bool at(unsigned v, int i) { return ((v >> i) & 1u) != 0; }
    };
    for (int i = 0; i <= 5; ++i) {
        setFunction(8, i, Bit::at(bits, i));
    }
    setFunction(8, 7, Bit::at(bits, 6));
    setFunction(8, 8, Bit::at(bits, 7));
    setFunction(7, 8, Bit::at(bits, 8));
    for (int i = 9; i < 15; ++i) {
        setFunction(14 - i, 8, Bit::at(bits, i));
    }
    for (int i = 0; i < 8; ++i) {
        setFunction(size_ - 1 - i, 8, Bit::at(bits, i));
    }
    for (int i = 8; i < 15; ++i) {
        setFunction(8, size_ - 15 + i, Bit::at(bits, i));
    }
    setFunction(8, size_ - 8, true);  // the dark module
}

void QrCode::drawVersion() {
    if (version_ < 7) {
        return;
    }
    unsigned rem = static_cast<unsigned>(version_);
    for (int i = 0; i < 12; ++i) {
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    }
    const unsigned bits = static_cast<unsigned>(version_) << 12 | rem;
    for (int i = 0; i < 18; ++i) {
        const bool bit = ((bits >> i) & 1u) != 0;
        const int a = size_ - 11 + i % 3, b = i / 3;
        setFunction(a, b, bit);
        setFunction(b, a, bit);
    }
}

void QrCode::drawCodewords(const uint8_t* codewords, size_t count) {
    size_t i = 0;
    for (int right = size_ - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5;  // skip the vertical timing pattern
        }
        for (int vert = 0; vert < size_; ++vert) {
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size_ - 1 - vert : vert;
                if (!function_[y][x] && i < count * 8) {
                    set(x, y, ((codewords[i >> 3] >> (7 - (i & 7))) & 1) != 0);
                    ++i;
                }
            }
        }
    }
}

void QrCode::applyMask(int mask) {
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            if (!function_[y][x] && maskedAt(mask, x, y)) {
                modules_[y][x] ^= 1;
            }
        }
    }
}

// ISO/IEC 18004 section 7.8.3: runs, 2 x 2 blocks, finder-like patterns, dark balance.
long QrCode::penalty() const {
    long total = 0;
    for (int pass = 0; pass < 2; ++pass) {  // rows, then columns
        for (int a = 0; a < size_; ++a) {
            int run = 0;
            int previous = -1;
            for (int b = 0; b < size_; ++b) {
                const int m = pass == 0 ? modules_[a][b] : modules_[b][a];
                if (m == previous) {
                    ++run;
                } else {
                    if (run >= 5) {
                        total += 3 + (run - 5);
                    }
                    run = 1;
                    previous = m;
                }
            }
            if (run >= 5) {
                total += 3 + (run - 5);
            }
            // 1:1:3:1:1 with four light modules on one side; outside the symbol counts as light.
            for (int b = 0; b < size_; ++b) {
                static const int kCore[7] = {1, 0, 1, 1, 1, 0, 1};
                bool core = true;
                for (int k = 0; k < 7 && core; ++k) {
                    core = lineAt(pass, a, b + k) == kCore[k];
                }
                if (!core) {
                    continue;
                }
                bool before = true, after = true;
                for (int k = 1; k <= 4; ++k) {
                    before = before && lineAt(pass, a, b - k) == 0;
                    after = after && lineAt(pass, a, b + 6 + k) == 0;
                }
                if (before) {
                    total += 40;
                }
                if (after) {
                    total += 40;
                }
            }
        }
    }
    int dark = 0;
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            dark += modules_[y][x];
            if (y + 1 < size_ && x + 1 < size_) {
                const int m = modules_[y][x];
                if (modules_[y][x + 1] == m && modules_[y + 1][x] == m && modules_[y + 1][x + 1] == m) {
                    total += 3;
                }
            }
        }
    }
    const int cells = size_ * size_;
    int diff = dark * 20 - cells * 10;
    if (diff < 0) {
        diff = -diff;
    }
    total += 10 * ((diff + cells - 1) / cells - 1);
    return total;
}

}  // namespace app
}  // namespace evo
