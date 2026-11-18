// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// MEDIA pressed while iDrive is on screen and Android Auto's sound plays on the car: Android Auto comes forward on its
// media app.
//
// With sound over Wi-Fi, Android Auto borrows CarPlay's media route (0x315, platform/audio_route.hpp), and iDrive then
// takes CarPlay for the playing source. Its MEDIA key asks CarPlay for its now-playing screen
// (CHmiHomeApplication::processHardKey, HmiMain3D 0x25c24c2, docs/aa-media-source-re.md), which does nothing without
// an iPhone. So the service asks the iDrive app to show Android Auto (kMediaJumpFile), and once the app says "show" it
// sends MEDIA to the phone, as when MEDIA is pressed over Android Auto. With the route given back, MEDIA is iDrive's.
//
// Pure: no clock, no file. The caller gives the time and holds its own lock. tests/test_media_key_jump.cpp.
#ifndef AA_ENTRYNAV_PLATFORM_MEDIA_KEY_JUMP_HPP
#define AA_ENTRYNAV_PLATFORM_MEDIA_KEY_JUMP_HPP

#include <stdint.h>

namespace evo {

class MediaKeyJump {
public:
    enum : uint32_t { kShowWithinMs = 4000 };  // a "show" later than this after the press is not the press's

    MediaKeyJump() : askedAt_(0), asked_(false) {}

    // MEDIA pressed with iDrive on screen. session: a session runs; route: our media route is built; blocked: a call
    // or reverse gear. True: ask the iDrive app to show Android Auto.
    bool pressed(bool session, bool route, bool blocked, uint32_t nowMs) {
        if (!session || !route || blocked) {
            return false;
        }
        asked_ = true;
        askedAt_ = nowMs;
        return true;
    }

    // The iDrive app said "show". True, once per press: send MEDIA to the phone.
    bool shown(uint32_t nowMs) {
        const bool mine = asked_ && nowMs - askedAt_ <= kShowWithinMs;
        asked_ = false;
        return mine;
    }

    // "hide", or the session ended: a later "show" is not for this press.
    void cancel() { asked_ = false; }

private:
    uint32_t askedAt_;
    bool asked_;
};

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_MEDIA_KEY_JUMP_HPP
