// Copyright (c) 2026 CallMeLewis
// QR codes (ISO/IEC 18004, model 2) for the Flare Drive page: bytes in byte mode, error correction level M, the
// smallest version that holds them (1 to 15, up to 77 x 77 modules), and the mask with the lowest penalty.
//
// Pure: no BMW call, no file. tests/test_qr_code.cpp compares the modules with a reference encoder's.
#ifndef AA_ENTRYNAV_HMIAPP_QR_CODE_HPP
#define AA_ENTRYNAV_HMIAPP_QR_CODE_HPP

#include <stddef.h>
#include <stdint.h>

namespace evo {
namespace app {

class QrCode {
public:
    static const int kMaxVersion = 15;
    static const int kMaxSize = 17 + 4 * kMaxVersion;
    static const int kQuietZone = 4;  // light modules the standard asks for around the code

    QrCode();

    // data: what the code holds. mask: 0 to 7, or -1 for the lowest penalty. False if it does not fit version 15.
    bool encode(const uint8_t* data, size_t size, int mask = -1);
    // The most bytes encode() takes.
    static size_t capacity(int version);

    int version() const { return version_; }
    int mask() const { return mask_; }
    int size() const { return size_; }  // modules per side, quiet zone not included; 0 before encode()
    bool dark(int x, int y) const { return modules_[y][x] != 0; }

private:
    void reset(int version);
    void set(int x, int y, bool dark) { modules_[y][x] = dark ? 1 : 0; }
    void setFunction(int x, int y, bool dark) {
        set(x, y, dark);
        function_[y][x] = 1;
    }
    void drawFunctionPatterns();
    void drawFinder(int x, int y);
    void drawAlignment(int x, int y);
    void drawFormat(int mask);
    void drawVersion();
    void drawCodewords(const uint8_t* codewords, size_t count);
    void applyMask(int mask);
    long penalty() const;
    // Module b of row a (pass 0) or of column a (pass 1); outside the symbol reads as light.
    int lineAt(int pass, int a, int b) const {
        if (b < 0 || b >= size_) {
            return 0;
        }
        return pass == 0 ? modules_[a][b] : modules_[b][a];
    }

    int version_, mask_, size_;
    uint8_t modules_[kMaxSize][kMaxSize];
    uint8_t function_[kMaxSize][kMaxSize];
};

}  // namespace app
}  // namespace evo

#endif
