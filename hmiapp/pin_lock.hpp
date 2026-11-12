// Copyright (c) 2026 CallMeLewis
// The PIN in front of the Flare Drive page (the QR code with the car's Wi-Fi): choosing it, typed twice the first time,
// then checking it. Five wrong PINs in a row lock the page for a minute, and each wrong PIN after that doubles the
// wait, up to an hour. The count is kept in the file, so restarting the head unit does not reset it; the wait starts
// again from the restart. The right PIN resets the count. A forgotten PIN: delete the file over SSH, then choose a new
// one.
//
// The file holds a salted hash, never the PIN: "pin1 <salt hex> <hash hex> <wrong PINs in a row>".
//
// Pure: no BMW call, no file, no clock (the caller passes monotonic seconds and the random salt).
// tests/test_pin_lock.cpp.
#ifndef AA_ENTRYNAV_HMIAPP_PIN_LOCK_HPP
#define AA_ENTRYNAV_HMIAPP_PIN_LOCK_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sha256.hpp"

namespace evo {
namespace app {

struct PinRecord {
    static const size_t kSaltSize = 16;
    bool set;  // false: no PIN chosen yet
    uint8_t salt[kSaltSize];
    uint8_t hash[Sha256::kSize];
    unsigned failures;  // wrong PINs in a row

    PinRecord() : set(false), failures(0) {
        memset(salt, 0, sizeof(salt));
        memset(hash, 0, sizeof(hash));
    }
};

// "pin1 <salt> <hash> <failures>\n" into out (cap >= kPinRecordText). False if it does not fit.
const size_t kPinRecordText = 5 + 2 * PinRecord::kSaltSize + 1 + 2 * Sha256::kSize + 1 + 10 + 2;
inline bool formatPinRecord(const PinRecord& r, char* out, size_t cap) {
    if (!r.set || cap < kPinRecordText) {
        return false;
    }
    static const char kHex[] = "0123456789abcdef";
    char* p = out;
    memcpy(p, "pin1 ", 5);
    p += 5;
    for (size_t i = 0; i < PinRecord::kSaltSize; ++i) {
        *p++ = kHex[r.salt[i] >> 4];
        *p++ = kHex[r.salt[i] & 15];
    }
    *p++ = ' ';
    for (size_t i = 0; i < Sha256::kSize; ++i) {
        *p++ = kHex[r.hash[i] >> 4];
        *p++ = kHex[r.hash[i] & 15];
    }
    snprintf(p, cap - static_cast<size_t>(p - out), " %u\n", r.failures);
    return true;
}

// The file's text back into r. False (r left unset) for anything else: the page then asks for a new PIN.
inline bool parsePinRecord(const char* text, PinRecord& r) {
    r = PinRecord();
    if (strncmp(text, "pin1 ", 5) != 0) {
        return false;
    }
    const char* p = text + 5;
    PinRecord parsed;
    struct Hex {
        static int digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
        static bool read(const char*& p, uint8_t* out, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                const int hi = digit(p[0]), lo = hi < 0 ? -1 : digit(p[1]);
                if (lo < 0) {
                    return false;
                }
                out[i] = static_cast<uint8_t>(hi << 4 | lo);
                p += 2;
            }
            return true;
        }
    };
    if (!Hex::read(p, parsed.salt, PinRecord::kSaltSize) || *p++ != ' ' || !Hex::read(p, parsed.hash, Sha256::kSize) ||
        *p++ != ' ') {
        return false;
    }
    unsigned long failures = 0;
    int digits = 0;
    for (; *p >= '0' && *p <= '9'; ++p, ++digits) {
        failures = failures * 10 + static_cast<unsigned long>(*p - '0');
        if (digits >= 6) {
            return false;
        }
    }
    if (digits == 0 || (*p != '\0' && *p != '\n')) {
        return false;
    }
    parsed.failures = static_cast<unsigned>(failures);
    parsed.set = true;
    r = parsed;
    return true;
}

class PinLock {
public:
    static const size_t kMinDigits = 4;
    static const size_t kMaxDigits = 8;
    static const unsigned kFreeTries = 5;   // wrong PINs in a row before the first lock
    static const long kFirstLockS = 60;     // the first lock; each wrong PIN after it doubles the wait
    static const long kMaxLockS = 60 * 60;  // the longest wait
    static const unsigned kRounds = 4096;   // hash rounds: slows a guess made from a copy of the file

    enum Mode { kChoose, kConfirm, kCheck };
    enum Result {
        kInvalid,     // not 4 to 8 digits: nothing counted
        kAskConfirm,  // a new PIN, to type again
        kMismatch,    // the second entry differed: choose again
        kChosen,      // the new PIN is saved (record() to write): the page opens
        kOpen,        // the right PIN: the page opens
        kWrong,       // a wrong PIN: triesLeft() before the next lock
        kLocked       // locked: lockedFor() seconds left, the PIN was not checked
    };

    // record: read from the file (unset when there is none); now: monotonic seconds, never 0.
    PinLock(const PinRecord& record, long now) : record_(record), lockedUntil_(0), mode_(kChoose), changed_(false) {
        memset(pending_, 0, sizeof(pending_));
        if (record_.set) {
            mode_ = kCheck;
            if (record_.failures >= kFreeTries) {
                lockedUntil_ = now + lockSeconds(record_.failures);  // a restart starts the wait again
            }
        }
    }

    Mode mode() const { return mode_; }
    long lockedFor(long now) const { return lockedUntil_ > now ? lockedUntil_ - now : 0; }
    unsigned triesLeft() const { return record_.failures >= kFreeTries ? 1 : kFreeTries - record_.failures; }

    // A PIN typed on the page. salt: random bytes, used only when a new PIN is saved.
    Result enter(const char* pin, long now, const uint8_t salt[PinRecord::kSaltSize]) {
        if (!valid(pin)) {
            return kInvalid;
        }
        switch (mode_) {
            case kChoose:
                strncpy(pending_, pin, kMaxDigits);
                mode_ = kConfirm;
                return kAskConfirm;
            case kConfirm: {
                const bool same = strcmp(pending_, pin) == 0;
                memset(pending_, 0, sizeof(pending_));
                if (!same) {
                    mode_ = kChoose;
                    return kMismatch;
                }
                record_.set = true;
                memcpy(record_.salt, salt, PinRecord::kSaltSize);
                digest(record_.salt, pin, record_.hash);
                record_.failures = 0;
                mode_ = kCheck;
                changed_ = true;
                return kChosen;
            }
            case kCheck:
                break;
        }
        if (lockedFor(now) > 0) {
            return kLocked;
        }
        uint8_t typed[Sha256::kSize];
        digest(record_.salt, pin, typed);
        uint8_t diff = 0;
        for (size_t i = 0; i < Sha256::kSize; ++i) {
            diff = static_cast<uint8_t>(diff | (typed[i] ^ record_.hash[i]));
        }
        if (diff == 0) {
            changed_ = changed_ || record_.failures != 0;
            record_.failures = 0;
            lockedUntil_ = 0;
            return kOpen;
        }
        ++record_.failures;
        changed_ = true;
        if (record_.failures >= kFreeTries) {
            lockedUntil_ = now + lockSeconds(record_.failures);
            return kLocked;
        }
        return kWrong;
    }

    // Leaving the page forgets a PIN chosen but not yet typed again.
    void cancel() {
        memset(pending_, 0, sizeof(pending_));
        if (mode_ == kConfirm) {
            mode_ = kChoose;
        }
    }

    // What to write to the file once enter() changed it; takeChanged() says so once.
    const PinRecord& record() const { return record_; }
    bool takeChanged() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }

    static bool valid(const char* pin) {
        size_t n = 0;
        for (; pin[n] != '\0'; ++n) {
            if (pin[n] < '0' || pin[n] > '9' || n >= kMaxDigits) {
                return false;
            }
        }
        return n >= kMinDigits;
    }

    // The wait after the failures-th wrong PIN in a row (failures >= kFreeTries).
    static long lockSeconds(unsigned failures) {
        long s = kFirstLockS;
        for (unsigned i = kFreeTries; i < failures && s < kMaxLockS; ++i) {
            s *= 2;
        }
        return s < kMaxLockS ? s : kMaxLockS;
    }

    static void digest(const uint8_t salt[PinRecord::kSaltSize], const char* pin, uint8_t out[Sha256::kSize]) {
        const size_t n = strlen(pin);
        Sha256 h;
        h.update(salt, PinRecord::kSaltSize);
        h.update(pin, n);
        h.finish(out);
        for (unsigned i = 1; i < kRounds; ++i) {
            Sha256 r;
            r.update(out, Sha256::kSize);
            r.update(salt, PinRecord::kSaltSize);
            r.update(pin, n);
            r.finish(out);
        }
    }

private:
    PinRecord record_;
    long lockedUntil_;  // monotonic seconds, 0: not locked
    Mode mode_;
    bool changed_;
    char pending_[kMaxDigits + 1];  // the new PIN between its two entries
};

}  // namespace app
}  // namespace evo

#endif
