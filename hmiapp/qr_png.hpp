// Copyright (c) 2026 CallMeLewis
// A QR code as a PNG file, the form the iDrive takes a picture in (bmw::Image, a raImageModel): side x side pixels,
// 8-bit RGB, black modules on white, the quiet zone included, centred, each module a whole number of pixels. Stored
// uncompressed (deflate's stored blocks), since the iDrive app links no zlib: 360 x 360 is about 390 KB, within the
// 1,024,000 bytes the iDrive accepts.
//
// Pure: no BMW call, no file. tests/test_qr_png.cpp reads the file back.
#ifndef AA_ENTRYNAV_HMIAPP_QR_PNG_HPP
#define AA_ENTRYNAV_HMIAPP_QR_PNG_HPP

#include <stddef.h>

#include "qr_code.hpp"

namespace evo {
namespace app {

// The bytes qrPng() writes for a side x side picture (side >= 1).
size_t qrPngSize(int side);

// The code into out, which holds qrPngSize(side) bytes. False if out is too small, or the code with its quiet zone
// does not fit side pixels at one pixel per module.
bool qrPng(const QrCode& code, int side, unsigned char* out, size_t cap);

}  // namespace app
}  // namespace evo

#endif
