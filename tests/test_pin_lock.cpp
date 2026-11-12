// Copyright (c) 2026 CallMeLewis
// hmiapp/pin_lock.hpp and hmiapp/sha256.hpp: the Flare Drive page's PIN, its lock and its file; SHA-256 against
// FIPS 180-4's examples.
#include <stdio.h>
#include <string.h>

#include "../hmiapp/pin_lock.hpp"

namespace {

using evo::app::PinLock;
using evo::app::PinRecord;
using evo::app::Sha256;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

const uint8_t kSalt[PinRecord::kSaltSize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

bool hexIs(const uint8_t* d, const char* hex) {
    char s[2 * Sha256::kSize + 1];
    for (size_t i = 0; i < Sha256::kSize; ++i) {
        snprintf(s + 2 * i, 3, "%02x", d[i]);
    }
    return strcmp(s, hex) == 0;
}

void sha256Examples() {
    uint8_t d[Sha256::kSize];
    Sha256::hash("", 0, d);
    CHECK(hexIs(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    Sha256::hash("abc", 3, d);
    CHECK(hexIs(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 448 bits: padding spills a block
    Sha256::hash(two, strlen(two), d);
    CHECK(hexIs(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    Sha256 h;  // a million 'a', fed in uneven pieces
    char a[997];
    memset(a, 'a', sizeof(a));
    size_t left = 1000000;
    while (left > 0) {
        const size_t n = left < sizeof(a) ? left : sizeof(a);
        h.update(a, n);
        left -= n;
    }
    h.finish(d);
    CHECK(hexIs(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

void choosingThenChecking() {
    PinLock lock(PinRecord(), 100);
    CHECK(lock.mode() == PinLock::kChoose);
    CHECK(lock.enter("1234", 100, kSalt) == PinLock::kAskConfirm);
    CHECK(lock.mode() == PinLock::kConfirm);
    CHECK(!lock.takeChanged());  // nothing to save until typed twice
    CHECK(lock.enter("1234", 101, kSalt) == PinLock::kChosen);
    CHECK(lock.takeChanged());
    CHECK(!lock.takeChanged());
    CHECK(lock.record().set);
    CHECK(lock.record().failures == 0);
    CHECK(memcmp(lock.record().salt, kSalt, sizeof(kSalt)) == 0);
    CHECK(lock.mode() == PinLock::kCheck);
    CHECK(lock.enter("1234", 102, kSalt) == PinLock::kOpen);
    CHECK(!lock.takeChanged());  // already at no failures: nothing to write
    CHECK(lock.enter("4321", 103, kSalt) == PinLock::kWrong);
    CHECK(lock.takeChanged());
    CHECK(lock.triesLeft() == PinLock::kFreeTries - 1);
    CHECK(lock.enter("1234", 104, kSalt) == PinLock::kOpen);  // the right PIN resets the count
    CHECK(lock.takeChanged());
    CHECK(lock.record().failures == 0);
}

void secondEntryDiffers() {
    PinLock lock(PinRecord(), 100);
    CHECK(lock.enter("1234", 100, kSalt) == PinLock::kAskConfirm);
    CHECK(lock.enter("1235", 101, kSalt) == PinLock::kMismatch);
    CHECK(lock.mode() == PinLock::kChoose);
    CHECK(!lock.record().set);
    CHECK(lock.enter("5678", 102, kSalt) == PinLock::kAskConfirm);
    lock.cancel();  // left the page between the two entries
    CHECK(lock.mode() == PinLock::kChoose);
    CHECK(lock.enter("5678", 103, kSalt) == PinLock::kAskConfirm);
    CHECK(lock.enter("5678", 104, kSalt) == PinLock::kChosen);
}

void onlyFourToEightDigits() {
    CHECK(!PinLock::valid(""));
    CHECK(!PinLock::valid("123"));
    CHECK(PinLock::valid("1234"));
    CHECK(PinLock::valid("12345678"));
    CHECK(!PinLock::valid("123456789"));
    CHECK(!PinLock::valid("12a4"));
    CHECK(!PinLock::valid("12 4"));
    PinLock lock(PinRecord(), 100);
    CHECK(lock.enter("12", 100, kSalt) == PinLock::kInvalid);
    CHECK(lock.mode() == PinLock::kChoose);
}

PinRecord chosen(const char* pin) {
    PinLock lock(PinRecord(), 1);
    lock.enter(pin, 1, kSalt);
    lock.enter(pin, 1, kSalt);
    return lock.record();
}

void locksAfterFiveWrong() {
    PinLock lock(chosen("2468"), 1000);
    for (unsigned i = 1; i < PinLock::kFreeTries; ++i) {
        CHECK(lock.enter("0000", 1000, kSalt) == PinLock::kWrong);
    }
    CHECK(lock.enter("0000", 1000, kSalt) == PinLock::kLocked);  // the fifth
    CHECK(lock.lockedFor(1000) == PinLock::kFirstLockS);
    CHECK(lock.enter("2468", 1030, kSalt) == PinLock::kLocked);  // locked: even the right PIN is not checked
    CHECK(lock.lockedFor(1030) == PinLock::kFirstLockS - 30);
    CHECK(lock.record().failures == PinLock::kFreeTries);
    CHECK(lock.triesLeft() == 1);
    CHECK(lock.enter("0000", 1060, kSalt) == PinLock::kLocked);  // one more wrong: twice the wait
    CHECK(lock.lockedFor(1060) == 2 * PinLock::kFirstLockS);
    CHECK(lock.enter("2468", 1060 + 2 * PinLock::kFirstLockS, kSalt) == PinLock::kOpen);
    CHECK(lock.record().failures == 0);
    CHECK(lock.lockedFor(1060 + 2 * PinLock::kFirstLockS) == 0);
}

void lockCapsAtAnHour() {
    CHECK(PinLock::lockSeconds(PinLock::kFreeTries) == 60);
    CHECK(PinLock::lockSeconds(PinLock::kFreeTries + 1) == 120);
    CHECK(PinLock::lockSeconds(PinLock::kFreeTries + 5) == 1920);
    CHECK(PinLock::lockSeconds(PinLock::kFreeTries + 6) == PinLock::kMaxLockS);
    CHECK(PinLock::lockSeconds(4000000000u) == PinLock::kMaxLockS);
}

void restartKeepsTheCount() {
    PinRecord r = chosen("1357");
    r.failures = PinLock::kFreeTries + 1;
    PinLock lock(r, 50);  // read back after a restart
    CHECK(lock.mode() == PinLock::kCheck);
    CHECK(lock.lockedFor(50) == 2 * PinLock::kFirstLockS);
    CHECK(lock.enter("1357", 51, kSalt) == PinLock::kLocked);
}

void fileRoundTrip() {
    PinRecord r = chosen("9753");
    r.failures = 7;
    char text[evo::app::kPinRecordText];
    CHECK(evo::app::formatPinRecord(r, text, sizeof(text)));
    CHECK(strncmp(text, "pin1 0102030405060708090a0b0c0d0e0f10 ", 38) == 0);
    CHECK(strstr(text, "9753") == nullptr);  // never the PIN itself
    PinRecord back;
    CHECK(evo::app::parsePinRecord(text, back));
    CHECK(back.set);
    CHECK(back.failures == 7);
    CHECK(memcmp(back.salt, r.salt, sizeof(r.salt)) == 0);
    CHECK(memcmp(back.hash, r.hash, sizeof(r.hash)) == 0);
    PinLock lock(back, 1);
    CHECK(lock.lockedFor(1) > 0);
    char small[10];
    CHECK(!evo::app::formatPinRecord(r, small, sizeof(small)));
    CHECK(!evo::app::formatPinRecord(PinRecord(), text, sizeof(text)));  // no PIN: nothing to write
}

void damagedFileAsksForANewPin() {
    PinRecord r;
    CHECK(!evo::app::parsePinRecord("", r));
    CHECK(!evo::app::parsePinRecord("pin2 00", r));
    CHECK(!evo::app::parsePinRecord("pin1 0102", r));
    char text[evo::app::kPinRecordText];
    evo::app::formatPinRecord(chosen("1111"), text, sizeof(text));
    text[10] = 'G';
    CHECK(!evo::app::parsePinRecord(text, r));
    CHECK(!r.set);
    evo::app::formatPinRecord(chosen("1111"), text, sizeof(text));
    char* count = strrchr(text, ' ');
    strcpy(count, " x\n");
    CHECK(!evo::app::parsePinRecord(text, r));
    strcpy(count, " 12345678\n");  // an absurd count is a damaged file too
    CHECK(!evo::app::parsePinRecord(text, r));
    strcpy(count, " 3");  // no line end: still read
    CHECK(evo::app::parsePinRecord(text, r));
    CHECK(r.failures == 3);
}

}  // namespace

int main() {
    sha256Examples();
    choosingThenChecking();
    secondEntryDiffers();
    onlyFourToEightDigits();
    locksAfterFiveWrong();
    lockCapsAtAnHour();
    restartKeepsTheCount();
    fileRoundTrip();
    damagedFileAsksForANewPin();
    if (g_failures) {
        printf("test_pin_lock: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pin_lock: OK\n");
    return 0;
}
