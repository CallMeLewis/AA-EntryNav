// Copyright (c) 2026 CallMeLewis
// When the app switches iDrive's sound to Bluetooth audio (the select-bt-audio file): once per phone connection, as it
// connects, so Android Auto's sound plays without picking the source by hand. Not with sound over Wi-Fi, which does not
// use Bluetooth. Never during a call or in reverse: it waits for them, up to 2 minutes, then leaves the source alone.
// The file is read when the phone connects; a change takes effect at the next connection.
//
// Pure: no BMW call, no file, no clock. tests/test_bt_audio_plan.cpp.
#ifndef AA_ENTRYNAV_HMIAPP_BT_AUDIO_PLAN_HPP
#define AA_ENTRYNAV_HMIAPP_BT_AUDIO_PLAN_HPP

namespace evo {
namespace app {

class BtAudioPlan {
public:
    enum Action { kNothing, kSwitch, kGiveUp };
    static const int kWaitS = 120;  // a call or reverse gear longer than this after connecting: the switch is dropped

    BtAudioPlan() : connectedAt_(0), decided_(false) {}

    // At each status poll. connected: a session with the phone; wanted: select-bt-audio present and audio-wifi absent;
    // busy: a call, or reverse gear; now: monotonic seconds (mono_clock.hpp), never 0.
    Action poll(bool connected, bool wanted, bool busy, long now) {
        if (!connected) {
            connectedAt_ = 0;
            decided_ = false;
            return kNothing;
        }
        if (connectedAt_ == 0) {
            connectedAt_ = now;
        }
        if (decided_) {
            return kNothing;
        }
        if (!wanted) {
            decided_ = true;  // read as the phone connected: a file written later waits for the next connection
            return kNothing;
        }
        if (busy) {
            if (now - connectedAt_ > kWaitS) {
                decided_ = true;
                return kGiveUp;
            }
            return kNothing;
        }
        decided_ = true;
        return kSwitch;
    }

private:
    long connectedAt_;  // first poll of this connection, 0: not connected
    bool decided_;      // switched, not wanted, or given up for this connection
};

}  // namespace app
}  // namespace evo

#endif
