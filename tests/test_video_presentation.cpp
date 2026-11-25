// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Actual EvoVideo lifecycle with fake hardware boundaries. No DRM, compositor or vehicle connection.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "evo_video.hpp"

static unsigned height = 0, opacityCommands = 0, decodeFailures = 0;
static unsigned srcWidth = 0, srcHeight = 0, streamWidth = 0, streamHeight = 0, outWidth = 0, outHeight = 0;
static uint32_t decoded = 0;                    // length of the last buffer handed to the decoder
static bool visible = false, poolBusy = false;  // poolBusy: the decoder holds every buffer
static float opacity = 1.f;
static uint8_t bufferMemory[256];
namespace evo {
// One buffer, free unless poolBusy: the fake decoder never holds it otherwise.
DmaBufPool::DmaBufPool() : drmFd_(-1), dev_(0) {}
DmaBufPool::~DmaBufPool() {}
bool DmaBufPool::init(unsigned, uint32_t) {
    Buf b = {nullptr, 7, bufferMemory, sizeof(bufferMemory), false};
    bufs_.assign(1, b);
    return true;
}
void DmaBufPool::destroy() {
    bufs_.clear();
}
int DmaBufPool::acquireFree() {
    return bufs_.empty() || poolBusy ? -1 : 0;
}
int DmaBufPool::find(int fd) {
    return !bufs_.empty() && fd == 7 ? 0 : -1;
}
EvoIlm::EvoIlm() {}
bool EvoIlm::validateTarget(uint32_t, uint32_t, uint32_t, uint32_t, bool) {
    return true;
}
bool EvoIlm::attachSurface(uint32_t, uint32_t, uint32_t, uint32_t h, uint32_t srcW, uint32_t srcH) {
    height = h;
    srcWidth = srcW;
    srcHeight = srcH;
    opacity = 1.f;
    visible = false;
    return true;
}
bool EvoIlm::detachSurface(uint32_t, uint32_t) {
    return true;
}
bool EvoIlm::setSurfaceVisibility(uint32_t, bool v) {
    visible = v;
    return true;
}
bool EvoIlm::setSurfaceOpacity(uint32_t, float v) {
    ++opacityCommands;
    opacity = v;
    return true;
}
}  // namespace evo
extern "C" {
xsv_handle xsv_open(int, uint32_t, const char*) {
    return reinterpret_cast<void*>(1);
}
void xsv_close(xsv_handle) {}
int xsv_configure_stream(xsv_handle, uint32_t w, uint32_t h, float) {
    streamWidth = w;
    streamHeight = h;
    return 0;
}
int xsv_set_output_dimension(xsv_handle, uint32_t w, uint32_t h) {
    outWidth = w;
    outHeight = h;
    return 0;
}
int xsv_enable_frame_events(xsv_handle) {
    return 0;
}
int xsv_update(xsv_handle) {
    return 0;
}
int xsv_start(xsv_handle) {
    return 0;
}
int xsv_stop(xsv_handle) {
    return 0;
}
int xsv_decode_video_frame(xsv_handle, int fd, uint32_t len) {
    assert(fd == 7);
    decoded = len;
    if (decodeFailures) {
        --decodeFailures;
        return -1;
    }
    return 0;
}
int xsv_receive_event(xsv_handle, int*, int) {
    usleep(1000);
    return 0;
}
}

static const uint8_t kSpsPps[] = {0, 0, 0, 1, 0x67, 1, 2, 3, 0, 0, 0, 1, 0x68, 4};
static const uint8_t kIdr[] = {0, 0, 0, 1, 0x65, 9, 9, 9, 9};
static const uint8_t kSlice[] = {0, 0, 0, 1, 0x41, 5, 5};

// The decoder received SPS/PPS followed by the IDR frame.
static bool configThenIdr() {
    return decoded == sizeof(kSpsPps) + sizeof(kIdr) && !memcmp(bufferMemory, kSpsPps, sizeof(kSpsPps)) &&
           !memcmp(bufferMemory + sizeof(kSpsPps), kIdr, sizeof(kIdr));
}

int main() {
    evo::EvoVideoConfig cfg;
    evo::EvoVideo video;
    assert(!video.isReady());
    assert(video.init(cfg) && video.start() && video.isReady() && height == 480);
    // 720p: decoded at 1280x720, its top 1280x480 on the panel unscaled.
    assert(!cfg.hd() && streamWidth == 1280 && streamHeight == 720 && outWidth == 1280 && outHeight == 720);
    assert(srcWidth == 1280 && srcHeight == 480);
    video.setVisible(true);
    assert(video.isVisible() && visible);
    assert(video.setOpacity(0.25f) && opacity == 0.25f);
    const unsigned n = opacityCommands;
    assert(video.setOpacity(0.25f) && opacityCommands == n);  // no repeated command on each main-loop tick
    assert(video.setOpacity(1.f) && opacity == 1.f);
    video.setVisible(false);  // camera/menu always hides the whole AA surface
    assert(!visible && !video.isVisible());
    video.setVisible(true);
    assert(visible);

    // SPS/PPS go in front of the first IDR; a frame the decoder refuses keeps them for the next IDR.
    video.postCodecConfig(kSpsPps, sizeof(kSpsPps));
    assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) && decoded == sizeof(kSlice));
    decodeFailures = 1;
    assert(!video.postVideoFrame(kIdr, sizeof(kIdr), 0) && configThenIdr());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) && configThenIdr());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) && decoded == sizeof(kIdr));  // sent once

    // A lost frame: a new IDR is asked for, at most every kKeyframeAskMs, until one reaches the decoder.
    assert(!video.takeKeyframeRequest(1000));
    decodeFailures = 1;
    assert(!video.postVideoFrame(kSlice, sizeof(kSlice), 0));
    assert(video.takeKeyframeRequest(1000));
    assert(!video.takeKeyframeRequest(1000 + evo::kKeyframeAskMs - 1));
    assert(video.takeKeyframeRequest(1000 + evo::kKeyframeAskMs));
    assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0));  // a slice does not end it
    assert(video.takeKeyframeRequest(1000 + 2 * evo::kKeyframeAskMs));
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0));  // an IDR does
    assert(!video.takeKeyframeRequest(1000 + 3 * evo::kKeyframeAskMs));
    // A lost IDR: the frames after it build on it, so a new one is asked for too.
    decodeFailures = 1;
    assert(!video.postVideoFrame(kIdr, sizeof(kIdr), 0));
    assert(video.takeKeyframeRequest(1000 + 4 * evo::kKeyframeAskMs));
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0));
    assert(!video.takeKeyframeRequest(1000 + 5 * evo::kKeyframeAskMs));

    // The decoder keeps every buffer: frames are dropped after a short wait, then, once nothing came back for
    // kVideoStallMs, at once, and the stall is reported once.
    poolBusy = true;
    assert(!video.postVideoFrame(kSlice, sizeof(kSlice), 0) && !video.takeStall());
    usleep((evo::kVideoStallMs + 200) * 1000);
    assert(!video.postVideoFrame(kSlice, sizeof(kSlice), 0));
    assert(video.takeStall() && !video.takeStall());
    assert(!video.takeKeyframeRequest(100000));  // no point asking a stuck decoder
    poolBusy = false;

    video.stop();
    assert(!visible && !video.isVisible() && !video.isReady());
    assert(!video.postVideoFrame(kIdr, sizeof(kIdr), 0));
    assert(video.init(cfg) && video.start() && video.isReady());
    assert(opacity == 1.f && video.setOpacity(0.25f) && opacity == 0.25f);   // fresh surface, fresh cache
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) && configThenIdr());  // the new decoder gets SPS/PPS again
    assert(!video.takeStall());                                              // and starts unstuck
    video.stop();

    // 1080p: decoded at 1920x1080, its top 1920x720 shrunk onto the 1280x480 panel.
    const evo::EvoVideoConfig hd(true);
    assert(hd.hd() && video.init(hd) && video.start() && video.isReady());
    assert(streamWidth == 1920 && streamHeight == 1080 && outWidth == 1920 && outHeight == 1080);
    assert(height == 480 && srcWidth == 1920 && srcHeight == 720);
    video.stop();
    // A part larger than the stream, or not of the panel's shape, is refused before anything starts.
    evo::EvoVideoConfig bad(true);
    bad.shownHeight = 1081;
    assert(!video.init(bad));
    bad.shownHeight = 400;
    assert(!video.init(bad));

    // A 1440x540 panel: on the cable 1920x1080 with its top 720 rows shrunk, its standard canvas (no hd one, so no
    // fallback to try); on Wi-Fi never wider than 1280x720, its top 480 rows stretched to the panel.
    const evo::EvoVideoConfig wide(true, 1440, 540);
    assert(!wide.hd() && wide.canvas.resolution == 3 && wide.bufSize == 2u << 20);
    assert(video.init(wide) && video.start());
    assert(streamWidth == 1920 && streamHeight == 1080 && height == 540 && srcWidth == 1920 && srcHeight == 720);
    video.stop();
    const evo::EvoVideoConfig wideWifi(false, 1440, 540, aa::kWifiCanvasWidth);
    assert(!wideWifi.hd() && wideWifi.canvas.resolution == 2 && wideWifi.bufSize == 1u << 20);
    assert(video.init(wideWifi) && video.start());
    assert(streamWidth == 1280 && streamHeight == 720 && height == 540 && srcWidth == 1280 && srcHeight == 480);
    video.stop();

    // An 800x400 panel: 800x480 decoded with its top 400 rows shown 1:1, or 1280x720 with its top 640 rows shrunk.
    const evo::EvoVideoConfig small(false, 800, 400);
    assert(!small.hd() && small.canvas.resolution == 1 && small.bufSize == 1u << 20);
    assert(video.init(small) && video.start());
    assert(streamWidth == 800 && streamHeight == 480 && height == 400 && srcWidth == 800 && srcHeight == 400);
    video.stop();
    const evo::EvoVideoConfig smallHd(true, 800, 400);
    assert(smallHd.hd() && smallHd.canvas.resolution == 2 && smallHd.bufSize == 2u << 20);
    assert(video.init(smallHd) && video.start());
    assert(streamWidth == 1280 && streamHeight == 720 && height == 400 && srcWidth == 1280 && srcHeight == 640);
    video.stop();
    // An 800x480 panel has no hd canvas; a screen no canvas fits gets a stream init() refuses.
    assert(!evo::EvoVideoConfig(true, 800, 480).hd() && evo::EvoVideoConfig(true, 800, 480).width == 800);
    // A 1920x720 panel: 1920x1080 is its standard canvas, with buffers big enough for a 1080p IDR frame.
    const evo::EvoVideoConfig full(true, 1920, 720);
    assert(!full.hd() && full.canvas.resolution == 3 && full.bufSize == 2u << 20);
    // The reference panel's hd canvas is still hd.
    assert(evo::EvoVideoConfig(true, 1280, 480).hd() && !evo::EvoVideoConfig(false, 1280, 480).hd());
    assert(!video.init(evo::EvoVideoConfig(false, 1024, 600)));

    // The Wi-Fi's frame pacing: off on the cable; on the Wi-Fi, a frame right behind the previous one waits for the
    // rest of the spacing, a frame that waited never makes the next one wait, and evenly spaced frames never wait.
    assert(evo::EvoVideoConfig().paceMs == 0 && evo::EvoVideoConfig(true).paceMs == 0);
    evo::FramePacer off;
    off.sent(1000, false);
    assert(off.holdMs(1001) == 0);
    evo::FramePacer pacer;
    pacer.spacingMs = evo::kWifiFrameSpacingMs;
    assert(pacer.holdMs(1000) == 0);  // the first frame
    pacer.sent(1000, false);
    assert(pacer.holdMs(1033) == 0);  // 30 frames a second: straight on
    pacer.sent(1033, false);
    assert(pacer.holdMs(1035) == 23);  // a pair: the second waits until 25 ms after the first
    pacer.sent(1058, true);
    assert(pacer.holdMs(1058) == 0);  // a third right behind it goes on: the waits never add up
    pacer.sent(1058, false);
    assert(pacer.holdMs(1060) == 23);
    assert(pacer.holdMs(0xFFFFFFF0u) == 0);  // long after (and across the clock's wrap): straight on
    puts(
        "PASS video presentation: visibility, opacity cache, camera hide, SPS/PPS after a refused IDR, a new IDR asked "
        "for after a lost frame, a stuck decoder reported once, reconnect, 720p shown 1:1 and 1080p shrunk onto the "
        "panel, 720p stretched to a wider panel, frame pacing on the Wi-Fi");
}
