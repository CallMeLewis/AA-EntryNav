// Copyright (c) 2026 CallMeLewis
// platform/audio_mixer.hpp: the phone's media and speech into the car's 48 kHz stereo. No sound library, no clock.
#include <stdio.h>

#include <vector>

#include "../platform/audio_mixer.hpp"

namespace {

using evo::AudioMixer;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

const size_t kMs = 48;  // frames per millisecond at 48 kHz

// `ms` of media, every sample `value`.
std::vector<int16_t> stereo(unsigned ms, int16_t value) {
    return std::vector<int16_t>(ms * kMs * 2, value);
}

// `ms` of mono at `rate`, every sample `value`.
std::vector<int16_t> mono(unsigned rate, unsigned ms, int16_t value) {
    return std::vector<int16_t>(rate / 1000 * ms, value);
}

void push(AudioMixer& m, AudioMixer::Stream s, const std::vector<int16_t>& samples, uint32_t now) {
    m.push(s, &samples[0], samples.size(), now);
}

// Mixes `ms` and returns it; empty when the mixer had nothing to play.
std::vector<int16_t> take(AudioMixer& m, unsigned ms, uint32_t now) {
    std::vector<int16_t> out(ms * kMs * 2, 12345);
    if (!m.mix(&out[0], ms * kMs, now)) {
        out.clear();
    }
    return out;
}

void prebufferAndFlush() {
    AudioMixer m;
    CHECK(m.configure(AudioMixer::kMedia, 48000, 2));
    CHECK(take(m, 10, 0).empty() && !m.pending());
    push(m, AudioMixer::kMedia, stereo(50, 700), 1000);
    CHECK(m.pending() && take(m, 10, 1010).empty());  // less than the prebuffer, and more may come
    push(m, AudioMixer::kMedia, stereo(50, 700), 1020);
    std::vector<int16_t> out = take(m, 10, 1025);  // 100 ms wait: it plays
    CHECK(out.size() == 10 * kMs * 2 && out[0] == 700 && out[out.size() - 1] == 700);
    CHECK(m.playing(AudioMixer::kMedia) && m.waiting(AudioMixer::kMedia) == 90 * kMs);
    out = take(m, 100, 1100);  // more asked than waits: the rest is silence, and the stream ran dry
    CHECK(out.size() == 100 * kMs * 2 && out[90 * kMs * 2 - 1] == 700 && out[90 * kMs * 2] == 0);
    CHECK(!m.playing(AudioMixer::kMedia) && m.stats().underruns[AudioMixer::kMedia] == 1);
    CHECK(take(m, 10, 1200).empty());

    // A short prompt never reaches the prebuffer: it plays once nothing more came for kFlushMs.
    CHECK(m.configure(AudioMixer::kSpeech, 48000, 1));
    push(m, AudioMixer::kSpeech, mono(48000, 30, 500), 2000);
    CHECK(take(m, 10, 2079).empty());
    out = take(m, 10, 2080);
    CHECK(out.size() == 10 * kMs * 2 && out[0] == 500 && out[1] == 500);
    // Or at once when the phone ended the stream; an ended stream running dry is no underrun.
    take(m, 100, 2090);
    CHECK(m.stats().underruns[AudioMixer::kSpeech] == 1);
    push(m, AudioMixer::kSpeech, mono(48000, 30, 500), 3000);
    m.end(AudioMixer::kSpeech);
    CHECK(take(m, 100, 3000).size() == 100 * kMs * 2);
    CHECK(m.stats().underruns[AudioMixer::kSpeech] == 1);
    CHECK(m.stats().framesIn[AudioMixer::kSpeech] == 60 * kMs && m.stats().framesOut == 320 * kMs);
}

void conversion() {
    AudioMixer m;
    CHECK(m.configure(AudioMixer::kSpeech, 16000, 1));
    const int16_t samples[] = {0, 300, 600, -300};
    m.push(AudioMixer::kSpeech, samples, 4, 0);
    m.end(AudioMixer::kSpeech);
    CHECK(m.waiting(AudioMixer::kSpeech) == 12);  // three frames out for each in
    int16_t out[24];
    CHECK(m.mix(out, 12, 0));
    const int16_t want[12] = {0, 0, 0, 100, 200, 300, 400, 500, 600, 300, 0, -300};  // straight lines between samples
    for (int i = 0; i < 12; ++i) {
        CHECK(out[i * 2] == want[i] && out[i * 2 + 1] == want[i]);  // mono on both sides
    }
    // The line goes on from the last sample of the previous push.
    const int16_t next[] = {300};
    m.push(AudioMixer::kSpeech, next, 1, 10);
    m.end(AudioMixer::kSpeech);
    CHECK(m.mix(out, 3, 10) && out[0] == -100 && out[2] == 100 && out[4] == 300);
    // 24 kHz stereo: two for one, each side on its own.
    AudioMixer media;
    CHECK(media.configure(AudioMixer::kMedia, 24000, 2));
    const int16_t pairs[] = {1000, -1000, 2000, -2000};
    media.push(AudioMixer::kMedia, pairs, 4, 20);
    media.end(AudioMixer::kMedia);
    CHECK(media.mix(out, 4, 20) && out[0] == 1000 && out[1] == -1000 && out[4] == 1500 && out[5] == -1500 &&
          out[6] == 2000 && out[7] == -2000);
    // A format the mixer does not convert stays silent.
    CHECK(!media.configure(AudioMixer::kMedia, 44100, 2) && !media.configure(AudioMixer::kMedia, 48000, 6));
    media.push(AudioMixer::kMedia, pairs, 4, 30);
    CHECK(!media.pending());
}

void ducking() {
    AudioMixer m(100, 30);
    m.configure(AudioMixer::kMedia, 48000, 2);
    m.configure(AudioMixer::kSpeech, 48000, 1);
    push(m, AudioMixer::kMedia, stereo(400, 10000), 0);
    std::vector<int16_t> out = take(m, 100, 0);
    CHECK(out[0] == 10000 && out[out.size() - 1] == 10000);  // alone: untouched
    push(m, AudioMixer::kSpeech, mono(48000, 200, 1000), 100);
    out = take(m, 200, 100);
    CHECK(out[0] > 10900 && out[0] <= 11000);  // the ramp starts from full media
    const int16_t ducked = out[150 * kMs * 2];
    CHECK(ducked >= 3999 && ducked <= 4001);  // 30 % of media, plus the speech
    CHECK(out[out.size() - 1] == ducked);
    // Speech over: media stays lowered for the hold, then ramps back.
    push(m, AudioMixer::kMedia, stereo(300, 10000), 300);
    out = take(m, 300, 300);
    const int16_t held = out[0];
    CHECK(held >= 2999 && held <= 3001 && out[out.size() - 1] == held);  // 300 ms into the 400 ms hold
    push(m, AudioMixer::kMedia, stereo(300, 10000), 600);
    out = take(m, 300, 600);
    CHECK(out[90 * kMs * 2] == held);
    CHECK(out[160 * kMs * 2] > held && out[160 * kMs * 2] < 10000);
    CHECK(out[290 * kMs * 2] == 10000);

    // The gain applies to the whole mix; the sum is clamped, never wrapped.
    AudioMixer half(50, 100);
    half.configure(AudioMixer::kMedia, 48000, 2);
    half.configure(AudioMixer::kSpeech, 48000, 1);
    push(half, AudioMixer::kMedia, stereo(200, 10000), 0);
    out = take(half, 10, 0);
    CHECK(out[0] == 5000);
    AudioMixer loud(100, 100);
    loud.configure(AudioMixer::kMedia, 48000, 2);
    loud.configure(AudioMixer::kSpeech, 48000, 1);
    push(loud, AudioMixer::kMedia, stereo(200, 30000), 0);
    push(loud, AudioMixer::kSpeech, mono(48000, 200, 30000), 0);
    out = take(loud, 10, 0);
    CHECK(out[0] == 32767 && out[1] == 32767);
    push(loud, AudioMixer::kMedia, stereo(200, -30000), 0);
    push(loud, AudioMixer::kSpeech, mono(48000, 200, -30000), 0);
    take(loud, 190, 0);
    out = take(loud, 10, 0);
    CHECK(out[0] == -32768);
}

void queueTooLong() {
    AudioMixer m;
    m.configure(AudioMixer::kMedia, 48000, 2);
    push(m, AudioMixer::kMedia, stereo(200, 1), 0);
    push(m, AudioMixer::kMedia, stereo(200, 2), 0);
    CHECK(m.waiting(AudioMixer::kMedia) == 400 * kMs && m.stats().dropped[AudioMixer::kMedia] == 0);
    push(m, AudioMixer::kMedia, stereo(100, 3), 0);  // 500 ms wait: the oldest 350 go
    CHECK(m.waiting(AudioMixer::kMedia) == 150 * kMs && m.stats().dropped[AudioMixer::kMedia] == 350 * kMs);
    std::vector<int16_t> out = take(m, 150, 0);
    CHECK(out[0] == 2 && out[50 * kMs * 2 - 1] == 2 && out[50 * kMs * 2] == 3);
    // The session ended: nothing waits, nothing plays.
    push(m, AudioMixer::kMedia, stereo(300, 4), 10);
    m.clear();
    CHECK(!m.pending() && take(m, 10, 20).empty());
    // Half a frame at the end of a push is dropped, not kept for the wrong side.
    const int16_t odd[] = {7, 7, 7};
    m.push(AudioMixer::kMedia, odd, 3, 30);
    CHECK(m.waiting(AudioMixer::kMedia) == 1);
}

}  // namespace

int main() {
    prebufferAndFlush();
    conversion();
    ducking();
    queueTooLong();
    if (g_failures) {
        printf("FAIL audio mixer: %d checks failed\n", g_failures);
        return 1;
    }
    puts(
        "PASS audio mixer: prebuffer and flush, 16 and 24 kHz to 48 kHz stereo, media lowered under speech, gain, "
        "clamp, queue too long, session end");
    return 0;
}
