// Copyright (c) 2026 CallMeLewis
// hmiapp/qr_code.cpp: the modules of a few codes against a reference encoder's (Python qrcode 7.4.2, byte mode, level
// M, the same version and mask), compared through the SHA-256 of their rows as "0"/"1"; capacities; refusals. Checked
// by hand against all 8 masks of 12 payloads from version 1 to 15 when the encoder was written.
#include <stdio.h>
#include <string.h>

#include <string>

#include "../hmiapp/qr_code.hpp"
#include "../hmiapp/sha256.hpp"

namespace {

using evo::app::QrCode;
using evo::app::Sha256;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

std::string modulesHash(const QrCode& q) {
    std::string rows;
    for (int y = 0; y < q.size(); ++y) {
        for (int x = 0; x < q.size(); ++x) {
            rows += q.dark(x, y) ? '1' : '0';
        }
    }
    uint8_t d[Sha256::kSize];
    Sha256::hash(rows.data(), rows.size(), d);
    char hex[2 * Sha256::kSize + 1];
    for (size_t i = 0; i < Sha256::kSize; ++i) {
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    }
    return hex;
}

struct Golden {
    std::string text;
    int mask, version;
    const char* hash;
};

void matchesTheReference() {
    const std::string url =
        "flaredrive://connect?v=1&ssid=BMW%2012345&psk=abcdefgh12345678abcdefgh&host=172.16.222.49&fp=SHA256:" +
        std::string(43, 'k');
    const Golden cases[] = {
        // version 1: one block, 8-bit count, pad bytes
        {"HELLO", 2, 1, "28bc72b3a74e226d925fc60e14807402ac1b5ae878e55637a20347d5a77284d4"},
        // version 8: version information, 4 blocks of 2 lengths
        {url, 5, 8, "7a1d0d06b9d2062361ab218a17acc09dd386b6d93907caabd405622bc5fdabc4"},
        // version 12: 16-bit count, 8 blocks
        {std::string(260, 'w'), 7, 12, "67838caef59b6101aa8f485a52920e6cb59c26d0624769ef5919fefee29c8129"},
        // version 15, full but for one pad byte
        {std::string(411, 'u'), 3, 15, "459f401c0564c71e8d7176230b3ece36cca361240563f09840496150286fbee3"},
    };
    static QrCode q;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const Golden& c = cases[i];
        CHECK(q.encode(reinterpret_cast<const uint8_t*>(c.text.data()), c.text.size(), c.mask));
        CHECK(q.version() == c.version);
        CHECK(q.mask() == c.mask);
        CHECK(q.size() == 17 + 4 * c.version);
        if (modulesHash(q) != c.hash) {
            printf("FAIL: %u bytes, mask %d: modules differ from the reference\n", static_cast<unsigned>(c.text.size()),
                   c.mask);
            ++g_failures;
        }
    }
}

void capacities() {
    CHECK(QrCode::capacity(0) == 0);
    CHECK(QrCode::capacity(1) == 14);
    CHECK(QrCode::capacity(9) == 180);
    CHECK(QrCode::capacity(10) == 213);  // 16-bit count from here
    CHECK(QrCode::capacity(15) == 412);
    CHECK(QrCode::capacity(16) == 0);
    static QrCode q;
    const std::string full(412, 'u'), over(413, 'u');
    CHECK(q.encode(reinterpret_cast<const uint8_t*>(full.data()), full.size()));
    CHECK(q.version() == 15);
    CHECK(!q.encode(reinterpret_cast<const uint8_t*>(over.data()), over.size()));
    CHECK(q.size() == 0);
    CHECK(!q.encode(reinterpret_cast<const uint8_t*>("x"), 1, 8));
    const std::string fourteen(14, 'a'), fifteen(15, 'a');
    CHECK(q.encode(reinterpret_cast<const uint8_t*>(fourteen.data()), fourteen.size()) && q.version() == 1);
    CHECK(q.encode(reinterpret_cast<const uint8_t*>(fifteen.data()), fifteen.size()) && q.version() == 2);
}

void picksAMask() {
    static QrCode q;
    CHECK(q.encode(reinterpret_cast<const uint8_t*>("HELLO"), 5));
    CHECK(q.mask() >= 0 && q.mask() <= 7);
    // Any mask's code is valid: the chosen one must equal that mask's code drawn on purpose.
    const std::string chosen = modulesHash(q);
    static QrCode again;
    CHECK(again.encode(reinterpret_cast<const uint8_t*>("HELLO"), 5, q.mask()));
    CHECK(modulesHash(again) == chosen);
}

}  // namespace

int main() {
    matchesTheReference();
    capacities();
    picksAMask();
    if (g_failures) {
        printf("test_qr_code: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_qr_code: OK\n");
    return 0;
}
