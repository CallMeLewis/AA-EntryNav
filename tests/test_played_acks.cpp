// Copyright (c) 2026 CallMeLewis
// Which media messages may be acked once their sound is played (core/played_acks.hpp), on the PC.
#include <stdio.h>
#include <stdlib.h>

#include "played_acks.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

const uint64_t kForceMs = 1000;

// A message is due once all of its sound was played, in the order the messages came.
void inOrder() {
    aa::PlayedAcks acks;
    CHECK(!acks.due() && acks.take(0, kForceMs) == 0 && !acks.played(100));
    acks.arrived(8192, 0);
    acks.arrived(8192, 40);
    CHECK(acks.waiting() == 2 && acks.mostWaiting() == 2 && !acks.due());
    CHECK(!acks.played(1920) && acks.take(50, kForceMs) == 0);  // 10 ms of it: not yet
    CHECK(acks.played(8192 - 1920) && acks.due());              // the first to its end
    CHECK(acks.take(60, kForceMs) == 1 && !acks.due() && acks.waiting() == 1);
    // One report that ends a message and starts the next; then one that ends two at once.
    acks.arrived(8192, 80);
    CHECK(acks.played(8192 + 100) && acks.take(90, kForceMs) == 1);
    acks.arrived(8192, 120);
    CHECK(acks.played(2 * 8192 - 100) && acks.take(130, kForceMs) == 2 && acks.waiting() == 0);
    CHECK(acks.forced() == 0 && acks.mostWaiting() == 2);
    // More played than any message holds (a stream that started over): it counts for nothing later.
    CHECK(!acks.played(5000));
    acks.arrived(1000, 200);
    CHECK(!acks.played(999) && acks.played(1) && acks.take(210, kForceMs) == 1);
}

// A message with no sound is due once those before it are; first in line, at once.
void empty() {
    aa::PlayedAcks acks;
    acks.arrived(0, 0);
    CHECK(acks.due() && acks.take(0, kForceMs) == 1);
    acks.arrived(100, 0);
    acks.arrived(0, 0);
    acks.arrived(100, 0);
    CHECK(!acks.due() && acks.played(100) && acks.take(0, kForceMs) == 2 && acks.waiting() == 1);
    CHECK(acks.played(100) && acks.take(0, kForceMs) == 1);
}

// Nobody plays: a message is acked at the limit, and its sound, played late, counts for none after it.
void forced() {
    aa::PlayedAcks acks;
    acks.arrived(1000, 0);
    acks.arrived(1000, 600);
    CHECK(!acks.played(400));
    CHECK(acks.take(999, kForceMs) == 0);
    CHECK(acks.take(1000, kForceMs) == 1 && acks.forced() == 1 && acks.waiting() == 1);  // the first, 600 left
    CHECK(!acks.played(600) && !acks.due());  // the late 600: not the second message's
    CHECK(!acks.played(999) && acks.played(1) && acks.take(1100, kForceMs) == 1 && acks.forced() == 1);
    // Both at the limit at once, with a played one behind them left to its turn.
    acks.arrived(500, 2000);
    acks.arrived(500, 2000);
    acks.arrived(500, 2900);
    CHECK(acks.take(3000, kForceMs) == 2 && acks.forced() == 3 && acks.waiting() == 1);
    CHECK(!acks.played(1000) && acks.played(500) && acks.take(3010, kForceMs) == 1);
}

// The stream starts over: all that waits is acked, and counted afresh. A new session forgets the counts too.
void restart() {
    aa::PlayedAcks acks;
    acks.arrived(1000, 0);
    acks.arrived(1000, 0);
    acks.played(300);
    CHECK(acks.takeAll() == 2 && acks.waiting() == 0 && !acks.due() && acks.forced() == 0);
    // The old stream's 1700 still to play count for the new one's messages: acked early, never late.
    acks.arrived(1000, 10);
    CHECK(acks.played(1700) && acks.take(20, kForceMs) == 1);
    acks.arrived(1000, 0);
    acks.take(5000, kForceMs);
    CHECK(acks.forced() == 1 && acks.mostWaiting() == 2);
    acks.clear();
    CHECK(acks.forced() == 0 && acks.mostWaiting() == 0 && acks.waiting() == 0);
    acks.arrived(1000, 0);
    CHECK(acks.played(1000) && acks.take(0, kForceMs) == 1);  // nothing late left over from before
}

}  // namespace

int main() {
    inOrder();
    empty();
    forced();
    restart();
    puts("PASS played acks: in order, part of a message, several at once, no sound, forced at the limit, start over");
    return 0;
}
