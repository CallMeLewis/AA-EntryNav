// Copyright (c) 2026 CallMeLewis
// UTF-8 into iDrive's UTF-16 (hmiapp/utf16.hpp): ASCII, accents, CJK, emoji pairs, bad input, the size limit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hmiapp/utf16.hpp"

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)

namespace {

size_t conv(const char* s, uint16_t* out, size_t max = 32) {
    return evo::app::utf8ToUtf16(s, strlen(s), out, max);
}

}  // namespace

int main() {
    uint16_t u[32];
    CHECK(conv("Abc", u) == 3 && u[0] == 'A' && u[2] == 'c');
    CHECK(conv("Ma\xc3\xb1"
               "ana",
               u) == 6 &&
          u[2] == 0xf1);                                                                  // n with tilde, two bytes
    CHECK(conv("\xe2\x80\x93\xe6\x97\xa5", u) == 2 && u[0] == 0x2013 && u[1] == 0x65e5);  // en dash, CJK
    CHECK(conv("\xf0\x9f\x8e\xb5", u) == 2 && u[0] == 0xd83c && u[1] == 0xdfb5);          // a note, as a pair
    CHECK(conv("a\x80"
               "b",
               u) == 3 &&
          u[1] == 0xfffd && u[2] == 'b');                                 // a stray continuation byte
    CHECK(conv("a\xc3", u) == 2 && u[1] == 0xfffd);                       // cut at the end
    CHECK(conv("\xc0\xaf", u) == 2 && u[0] == 0xfffd && u[1] == 0xfffd);  // overlong '/'
    CHECK(conv("\xed\xa0\x80", u) == 3 && u[0] == 0xfffd);                // a lone surrogate
    CHECK(conv("abcdef", u, 4) == 4 && u[3] == 'd');                      // the limit
    CHECK(conv("ab\xf0\x9f\x8e\xb5", u, 3) == 2);                         // never half a pair
    CHECK(evo::app::utf8ToUtf16("", 0, u, 32) == 0);
    puts("PASS utf16: ASCII, accents, CJK and surrogate pairs; bad, cut and overlong input as U+FFFD; the size limit");
    return 0;
}
