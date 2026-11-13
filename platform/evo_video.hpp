// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto video: H.264 decoded by libxsvideo onto an IVI surface, shown and hidden through the LayerManager.

// The pieces:
//   libxsvideo type 1   H.264 decoder (IVA-HD/DCE) rendering NV12 onto an IVI surface; it connects to Wayland
//                       itself (see xsv_api.h).
//   OMAP dma-bufs       carry the Annex B H.264 stream to the decoder (omap_dmabuf.hpp).
//   LayerManager (ILM)  attaches and hides our surface in a layer that is already visible (evo_ilm.hpp).
//
// The phone renders a 1280x720 or a 1920x1080 canvas (core/sdr_builder.hpp: canvasFor); the panel is 1280x480
// (omapdrm fb0 1280,480). The surface shows the canvas's top lines, which hold the phone's UI: 1280x480 of them 1:1,
// or 1920x720 shrunk onto the panel by the compositor.
#ifndef AA_ENTRYNAV_PLATFORM_EVO_VIDEO_HPP
#define AA_ENTRYNAV_PLATFORM_EVO_VIDEO_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <vector>

#include "evo_ilm.hpp"
#include "omap_dmabuf.hpp"
#include "xsv_api.h"

namespace evo {

// IVI ids of our surfaces (observed working on the car): Android Auto on the HMI's full-screen layer, the id checked
// free before each start. The volume pill takes the next id, above Android Auto.
const uint32_t kAaSurfaceId = 0x7eaa0001, kPillSurfaceId = 0x7eaa0002, kAaLayerId = 0x80000000;

struct EvoVideoConfig {
    uint32_t width, height;                    // phone stream: 1280x720, or 1920x1080 with hd
    uint32_t shownWidth, shownHeight;          // its top-left part on the panel: 1280x480, or 1920x720 with hd
    uint32_t displayWidth, displayHeight;      // the panel: 1280x480
    uint32_t fps;                              // 0 = not imposed
    uint32_t surfaceId;                        // kAaSurfaceId
    uint32_t layerId;                          // kAaLayerId, already visible
    unsigned bufCount;                         // 8 dma-bufs
    uint32_t bufSize;                          // 1 MiB each, 2 MiB with hd (an IDR frame is larger)
    explicit EvoVideoConfig(bool hd = false);  // this car's values, for the 720p or the 1080p canvas
    bool hd() const { return width > displayWidth; }
};

// What the picture path did since the last "[Video] last ..." log line: where a slow picture loses its time (the
// phone sending late, no free buffer, the decoder call, or the compositor). Kept under EvoVideo's mutex.
const uint32_t kVideoStatsMs = 10000;
const uint32_t kVideoStallMs = 3000;   // no buffer given back by the decoder for this long: it is stuck
const uint32_t kKeyframeAskMs = 2000;  // a new IDR is asked for at most this often
struct VideoStats {
    uint32_t sinceMs;                             // start of the stretch (evo::monoMs)
    uint32_t lastFrameMs;                         // previous frame from the phone, 0 before the first
    unsigned fromPhone, dropped, decoded, shown;  // frames received, dropped, handed to the decoder, shown
    uint32_t maxGapMs, maxBufferWaitMs, maxDecodeCallMs;
    VideoStats()
        : sinceMs(0),
          lastFrameMs(0),
          fromPhone(0),
          dropped(0),
          decoded(0),
          shown(0),
          maxGapMs(0),
          maxBufferWaitMs(0),
          maxDecodeCallMs(0) {}
    // A new stretch from nowMs; the time of the last frame is kept, so a gap across two stretches still counts.
    void reset(uint32_t nowMs) {
        const uint32_t last = lastFrameMs;
        *this = VideoStats();
        sinceMs = nowMs;
        lastFrameMs = last;
    }
    // One frame from the phone at nowMs: counted, and the longest gap between two frames kept.
    void frame(uint32_t nowMs) {
        if (lastFrameMs && nowMs - lastFrameMs > maxGapMs) {
            maxGapMs = nowMs - lastFrameMs;
        }
        lastFrameMs = nowMs ? nowMs : 1;  // 0 means "none yet"
        ++fromPhone;
    }
    bool due(uint32_t nowMs) const { return fromPhone > 0 && nowMs - sinceMs >= kVideoStatsMs; }
};

class EvoVideo {
public:
    EvoVideo();
    ~EvoVideo();

    bool init(const EvoVideoConfig& cfg);
    bool start();  // opens and configures libxsvideo, starts the event thread, attaches the hidden surface
    void stop();

    void setVisible(bool visible);
    bool setOpacity(float opacity);  // our surface only; cached (no CLI call when unchanged)
    bool isVisible() const { return visible_; }
    bool isReady() const { return ready_; }  // start() completed: the surface is attached and can be shown
    bool firstFrameSeen();                   // compositor frame callback after H.264; not a visual confirmation

    // SPS/PPS (the Android Auto codec config): kept and prepended to the first IDR frame after each (re)start.
    void postCodecConfig(const uint8_t* data, size_t len);
    // One H.264 access unit, copied into a free dma-buf and handed to the decoder.
    // False when no buffer frees up in time (frame dropped) or the stream is not started.
    bool postVideoFrame(const uint8_t* data, size_t len, uint64_t pts);

    // Once, when the decoder has held every buffer for kVideoStallMs: it gave none back (after an error, a watchdog or
    // an abort, libxsvideo may send no completion), so the picture would stay frozen for the session. The caller ends
    // the session; the next one starts a new decoder. Until then frames are dropped at once, not after a wait.
    bool takeStall();
    // A frame was dropped since the last IDR, so the picture smears until the phone sends one: true at most every
    // kKeyframeAskMs while that lasts. The caller asks the phone for one (video focus, which makes it start afresh).
    bool takeKeyframeRequest(uint32_t nowMs);

private:
    static void* eventStub(void* self);
    void eventLoop();
    bool isIdr(const uint8_t* data, size_t len) const;  // holds a NAL unit of type 5?

    EvoVideoConfig cfg_;
    EvoIlm ilm_;
    DmaBufPool pool_;
    xsv_handle h_;
    std::atomic<bool> running_;
    std::atomic<bool> visible_;
    float opacity_;
    bool ready_;
    bool started_;
    bool configSent_;
    pthread_t evThread_;
    bool evStarted_;
    pthread_mutex_t mu_;
    pthread_cond_t cv_;              // signalled when a dma-buf is released; waits use CLOCK_MONOTONIC
    std::vector<uint8_t> codecCfg_;  // cached SPS/PPS
    bool seenFirstFrame_;
    uint64_t framesIn_, framesDropped_, idrSeen_, framesAtStart_;
    uint32_t lastReleaseMs_;  // a buffer last came back from the decoder (or the stream started); under mu_
    bool stalled_, stallTaken_;
    bool needKeyframe_;       // a frame was dropped since the last IDR handed to the decoder
    uint32_t keyframeAskMs_;  // the last request (0: none)
    VideoStats stats_;        // under mu_
    // Logs the stretch and starts a new one when it is due. Called with mu_ held; releases it around the log line.
    void logStatsIfDue(uint32_t nowMs);
};

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_EVO_VIDEO_HPP
