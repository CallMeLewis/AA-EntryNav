// Copyright (c) 2026 CallMeLewis
// hmiapp/bt_audio_plan.hpp: when the app switches iDrive's sound to Bluetooth audio, connection by connection.
#include <stdio.h>

#include "../hmiapp/bt_audio_plan.hpp"

namespace {

using evo::app::BtAudioPlan;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

void oncePerConnection() {
    BtAudioPlan p;
    CHECK(p.poll(false, true, false, 100) == BtAudioPlan::kNothing);  // no phone yet
    CHECK(p.poll(true, true, false, 101) == BtAudioPlan::kSwitch);    // the phone connects: switch
    CHECK(p.poll(true, true, false, 102) == BtAudioPlan::kNothing);   // once only
    CHECK(p.poll(true, true, false, 500) == BtAudioPlan::kNothing);
    CHECK(p.poll(false, true, false, 501) == BtAudioPlan::kNothing);  // it drops
    CHECK(p.poll(true, true, false, 600) == BtAudioPlan::kSwitch);    // the next connection switches again
}

void notWanted() {
    BtAudioPlan p;
    CHECK(p.poll(true, false, false, 100) == BtAudioPlan::kNothing);
    // The file written during the session waits for the next connection.
    CHECK(p.poll(true, true, false, 101) == BtAudioPlan::kNothing);
    CHECK(p.poll(false, true, false, 102) == BtAudioPlan::kNothing);
    CHECK(p.poll(true, true, false, 103) == BtAudioPlan::kSwitch);
}

void waitsForCallOrReverse() {
    BtAudioPlan p;
    CHECK(p.poll(true, true, true, 100) == BtAudioPlan::kNothing);  // in a call as the phone connects
    CHECK(p.poll(true, true, true, 150) == BtAudioPlan::kNothing);
    CHECK(p.poll(true, true, false, 151) == BtAudioPlan::kSwitch);  // the call ended: switch now
    CHECK(p.poll(true, true, false, 152) == BtAudioPlan::kNothing);
}

void givesUpAfterTwoMinutes() {
    BtAudioPlan p;
    CHECK(p.poll(true, true, true, 100) == BtAudioPlan::kNothing);
    CHECK(p.poll(true, true, true, 100 + BtAudioPlan::kWaitS) == BtAudioPlan::kNothing);
    CHECK(p.poll(true, true, true, 101 + BtAudioPlan::kWaitS) == BtAudioPlan::kGiveUp);
    CHECK(p.poll(true, true, false, 102 + BtAudioPlan::kWaitS) == BtAudioPlan::kNothing);  // given up: left alone
}

}  // namespace

int main() {
    oncePerConnection();
    notWanted();
    waitsForCallOrReverse();
    givesUpAfterTwoMinutes();
    if (g_failures) {
        printf("FAIL bluetooth audio plan: %d checks failed\n", g_failures);
        return 1;
    }
    puts("PASS bluetooth audio plan: once per connection, read as the phone connects, waits for a call or reverse");
    return 0;
}
