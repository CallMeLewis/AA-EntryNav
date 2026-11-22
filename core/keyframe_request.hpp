// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// When the head unit may ask the phone for a new IDR after the decoder lost a video frame.
#ifndef AA_ENTRYNAV_CORE_KEYFRAME_REQUEST_HPP
#define AA_ENTRYNAV_CORE_KEYFRAME_REQUEST_HPP

#include <stdint.h>

namespace aa {

// The frames between two IDRs build on the ones before them: once one is lost, the picture stays broken until the
// phone's next IDR, which can be seconds away. Android Auto has no message that asks for one. A phone whose video focus
// is taken away and given back starts its encoder again, with an IDR (upstream EntryNav2-Android-Auto #81), so the
// session sends UNFOCUSED then FOCUSED (aa_session.cpp). A FOCUSED alone, which the phone already has, was never seen
// to bring one in a car log.
// Not in the first seconds after the video is set up, while the phone's own first IDRs still come, and seldom: a
// decoder short of buffers would otherwise turn every frame into an IDR, each larger than the frames it replaces.
// Session thread only; the clock is the caller's, in milliseconds.
class KeyframeRequest {
public:
    static const uint64_t kSettleMs = 5000;    // after the video was set up
    static const uint64_t kSpacingMs = 10000;  // between two requests

    KeyframeRequest() : videoSinceMs_(0), lastMs_(0), video_(false), asked_(false) {}

    // The phone set the video channel up: a new stream, nothing asked of it yet.
    void videoStarted(uint64_t nowMs) {
        video_ = true;
        videoSinceMs_ = nowMs;
        asked_ = false;
    }
    // A new session.
    void reset() { video_ = asked_ = false; }
    // A frame was lost: true when a request may go now, and it then counts as made.
    bool allow(uint64_t nowMs) {
        if (!video_ || nowMs - videoSinceMs_ < kSettleMs || (asked_ && nowMs - lastMs_ < kSpacingMs)) {
            return false;
        }
        asked_ = true;
        lastMs_ = nowMs;
        return true;
    }

private:
    uint64_t videoSinceMs_, lastMs_;
    bool video_, asked_;
};

}  // namespace aa

#endif  // AA_ENTRYNAV_CORE_KEYFRAME_REQUEST_HPP
