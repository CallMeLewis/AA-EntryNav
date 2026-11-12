// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Sound over Wi-Fi: settings, and the thread that plays the mix on the car (see evo_audio.hpp).
#include "evo_audio.hpp"

#include <errno.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "aa_protocol.hpp"
#include "evo_log.hpp"

namespace evo {

namespace {
const char* const kTag = "Audio";
const size_t kPeriodFrames = 480;         // 10 ms per write
const unsigned kLatencyUs = 100000;       // libasound's buffer
const uint32_t kDrainAfterMs = 60;        // nothing to play this long: what was written plays to its end
const uint32_t kCloseAfterMs = 5000;      // then the PCM is given back
const uint32_t kRetryOpenAfterMs = 2000;  // a PCM that did not open is not tried again before this

// The formats of the description sent to the phone (core/include/dhu_sdp.h, as core/sdr_builder.cpp leaves it):
// channel 9 has one configuration, channel 8 two.
bool formatOf(uint8_t channel, uint32_t config, AudioMixer::Stream& stream, unsigned& rate, unsigned& channels) {
    if (channel == aa::channel::kMediaAudio && config == 0) {
        stream = AudioMixer::kMedia;
        rate = 48000;
        channels = 2;
        return true;
    }
    if (channel == aa::channel::kGuidanceAudio && config <= 1) {
        stream = AudioMixer::kSpeech;
        rate = config == 0 ? 16000 : 48000;
        channels = 1;
        return true;
    }
    return false;
}

bool streamOf(uint8_t channel, AudioMixer::Stream& stream) {
    if (channel != aa::channel::kMediaAudio && channel != aa::channel::kGuidanceAudio) {
        return false;
    }
    stream = channel == aa::channel::kMediaAudio ? AudioMixer::kMedia : AudioMixer::kSpeech;
    return true;
}

// 0 to 100, else `keep`.
int percentOr(const std::string& text, int keep) {
    if (text.empty() || text.size() > 3) {
        return keep;
    }
    int value = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return keep;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value <= 100 ? value : keep;
}
}  // namespace

bool validPcmName(const std::string& name) {
    if (name.empty() || name.size() > 63) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.' || c == ':' || c == ',' || c == '=';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool loadAudioSettings(const char* path, AudioSettings& settings) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return false;
    }
    char text[96];
    while (fgets(text, sizeof(text), file)) {
        std::string line(text);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) {
            line.erase(line.size() - 1);
        }
        if (line.compare(0, 4, "pcm=") == 0 && validPcmName(line.substr(4))) {
            settings.pcm = line.substr(4);
        } else if (line.compare(0, 5, "gain=") == 0) {
            settings.gainPercent = percentOr(line.substr(5), settings.gainPercent);
        } else if (line.compare(0, 5, "duck=") == 0) {
            settings.duckPercent = percentOr(line.substr(5), settings.duckPercent);
        }
    }
    fclose(file);
    return true;
}

EvoAudio::EvoAudio() : thread_(0), started_(false), quit_(false) {
    pthread_mutex_init(&mu_, nullptr);
    // Timed waits on the monotonic clock: the car's wall clock jumps when its GPS sets it.
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cv_, &attr);
    pthread_condattr_destroy(&attr);
}

EvoAudio::~EvoAudio() {
    stop();
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
}

bool EvoAudio::start(const AudioSettings& settings) {
    if (started_) {
        return true;
    }
    if (!out_.load()) {
        ELOG(kTag, "%s: no sound over Wi-Fi", out_.lastError().c_str());
        return false;
    }
    settings_ = settings;
    mixer_ = AudioMixer(settings.gainPercent, settings.duckPercent);
    quit_ = false;
    if (pthread_create(&thread_, nullptr, threadStub, this) != 0) {
        ELOG(kTag, "cannot start the thread: no sound over Wi-Fi");
        return false;
    }
    started_ = true;
    ELOG(kTag, "ready: PCM '%s', gain %d %%, media at %d %% under speech", settings_.pcm.c_str(), settings_.gainPercent,
         settings_.duckPercent);
    return true;
}

void EvoAudio::stop() {
    if (!started_) {
        return;
    }
    pthread_mutex_lock(&mu_);
    quit_ = true;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
    pthread_join(thread_, nullptr);
    started_ = false;
}

void EvoAudio::stream(uint8_t channel, bool started, uint32_t config) {
    AudioMixer::Stream s = AudioMixer::kMedia;
    unsigned rate = 0, channels = 0;
    if (started) {
        const bool known = formatOf(channel, config, s, rate, channels);
        if (!known) {
            // Silent rather than at the wrong speed.
            ELOG(kTag, "channel %u configuration %u: format not known, stream not played", channel, config);
            if (!streamOf(channel, s)) {
                return;
            }
        }
        pthread_mutex_lock(&mu_);
        mixer_.configure(s, rate, channels);
        pthread_mutex_unlock(&mu_);
        if (known) {
            ELOG(kTag, "%s starts: %u Hz, %u channel(s)", s == AudioMixer::kMedia ? "media" : "speech", rate, channels);
        }
        return;
    }
    if (!streamOf(channel, s)) {
        return;
    }
    pthread_mutex_lock(&mu_);
    mixer_.end(s);
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
    ELOG(kTag, "%s stops", s == AudioMixer::kMedia ? "media" : "speech");
}

void EvoAudio::data(uint8_t channel, const uint8_t* pcm, size_t len) {
    AudioMixer::Stream s = AudioMixer::kMedia;
    if (!streamOf(channel, s) || len < 2) {
        return;
    }
    // The phone's bytes are little-endian samples at any address: copied into place first.
    std::vector<int16_t> samples(len / 2);
    memcpy(&samples[0], pcm, samples.size() * 2);
    pthread_mutex_lock(&mu_);
    if (started_ && !quit_) {
        mixer_.push(s, &samples[0], samples.size(), monoMs());
        pthread_cond_signal(&cv_);
    }
    pthread_mutex_unlock(&mu_);
}

void EvoAudio::sessionEnded() {
    pthread_mutex_lock(&mu_);
    mixer_.clear();
    pthread_mutex_unlock(&mu_);
    logStats("session ended");
}

void EvoAudio::onData(uint8_t channel, const uint8_t* pcm, size_t len, uint64_t, void* user) {
    static_cast<EvoAudio*>(user)->data(channel, pcm, len);
}

void EvoAudio::onStream(uint8_t channel, bool started, uint32_t config, void* user) {
    static_cast<EvoAudio*>(user)->stream(channel, started, config);
}

void EvoAudio::logStats(const char* when) {
    const AudioMixer::Stats stats = this->stats();
    ELOG(kTag,
         "%s: media %lu ms in (%lu ms lost, %lu gap(s)), speech %lu ms in (%lu ms lost, %lu gap(s)), %lu ms played",
         when, stats.framesIn[AudioMixer::kMedia] / 48, stats.dropped[AudioMixer::kMedia] / 48,
         stats.underruns[AudioMixer::kMedia], stats.framesIn[AudioMixer::kSpeech] / 48,
         stats.dropped[AudioMixer::kSpeech] / 48, stats.underruns[AudioMixer::kSpeech], stats.framesOut / 48);
}

void* EvoAudio::threadStub(void* arg) {
    static_cast<EvoAudio*>(arg)->loop();
    return nullptr;
}

// The PCM's buffer filled with silence. An empty buffer takes the first sound all at once, without waiting: the queues
// run dry at once and the stream stops and starts. Full, the PCM plays, and every write after it waits for the car's
// own sound clock, which is what paces the mix.
bool EvoAudio::prime() {
    const int16_t silence[kPeriodFrames * kMixChannels] = {0};
    for (size_t left = out_.bufferFrames(); left;) {
        const size_t frames = left < kPeriodFrames ? left : kPeriodFrames;
        if (!out_.write(silence, frames)) {
            return false;
        }
        left -= frames;
    }
    return true;
}

AudioMixer::Stats EvoAudio::stats() {
    pthread_mutex_lock(&mu_);
    const AudioMixer::Stats stats = mixer_.stats();
    pthread_mutex_unlock(&mu_);
    return stats;
}

void EvoAudio::loop() {
    int16_t period[kPeriodFrames * kMixChannels];
    uint32_t wroteAt = 0;    // last write
    uint32_t refusedAt = 0;  // last open that failed
    bool refused = false;
    bool toDrain = false;    // written since the last drain
    bool primed = false;     // the PCM's buffer was filled since it last ran empty (open, drain, underrun)
    unsigned underruns = 0;  // the PCM's count when it was last primed
    for (;;) {
        pthread_mutex_lock(&mu_);
        bool have = false;
        if (!quit_) {
            have = mixer_.mix(period, kPeriodFrames, monoMs());
            if (!have) {
                // Queued but not playing yet: its prebuffer or its flush delay is close. Else wait for sound, waking
                // for the drain and the close of an open PCM.
                const long waitMs = mixer_.pending() ? 5 : toDrain ? 20 : out_.isOpen() ? 500 : 60000;
                struct timespec until;
                clock_gettime(CLOCK_MONOTONIC, &until);
                until.tv_nsec += (waitMs % 1000) * 1000000L;
                until.tv_sec += waitMs / 1000 + until.tv_nsec / 1000000000L;
                until.tv_nsec %= 1000000000L;
                pthread_cond_timedwait(&cv_, &mu_, &until);
            }
        }
        const bool quit = quit_;
        pthread_mutex_unlock(&mu_);
        if (quit) {
            break;
        }
        const uint32_t now = monoMs();
        if (!have) {
            if (toDrain && now - wroteAt >= kDrainAfterMs) {
                out_.drain();
                toDrain = false;
                primed = false;
            }
            if (out_.isOpen() && !toDrain && now - wroteAt >= kCloseAfterMs) {
                out_.close();
                ELOG(kTag, "PCM '%s' closed after %u ms of silence", settings_.pcm.c_str(), kCloseAfterMs);
            }
            continue;
        }
        if (!out_.isOpen()) {
            if (refused && now - refusedAt < kRetryOpenAfterMs) {
                continue;  // this period is lost: the queue must not fill up behind a PCM that does not open
            }
            if (!out_.open(settings_.pcm.c_str(), kMixRate, kMixChannels, kLatencyUs)) {
                if (!refused) {
                    ELOG(kTag, "cannot open PCM '%s' (%s): no sound, tried again every %u ms", settings_.pcm.c_str(),
                         out_.lastError().c_str(), kRetryOpenAfterMs);
                }
                refused = true;
                refusedAt = now;
                continue;
            }
            refused = false;
            primed = false;
            ELOG(kTag, "PCM '%s' open: 48 kHz stereo, buffer %u ms", settings_.pcm.c_str(),
                 static_cast<unsigned>(out_.bufferFrames() * 1000 / kMixRate));
        }
        bool written = true;
        if (!primed) {
            underruns = out_.underruns();
            written = prime();
            primed = true;
        }
        if (!written || !out_.write(period, kPeriodFrames)) {
            ELOG(kTag, "PCM '%s' lost (%s): closed, opened again at the next sound", settings_.pcm.c_str(),
                 out_.lastError().c_str());
            out_.close();
            refused = true;
            refusedAt = now;
            toDrain = false;
            continue;
        }
        wroteAt = monoMs();
        toDrain = true;
        if (out_.underruns() != underruns) {
            primed = false;  // the PCM ran empty: filled again before the next sound
        }
    }
    if (toDrain) {
        out_.drain();
    }
    out_.close();
}

}  // namespace evo
