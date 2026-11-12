// Copyright (c) 2026 CallMeLewis
// Sound over Wi-Fi: the phone's media (AA channel 9) and its speech and guidance (channel 8), played on the car.
//
// Off unless the settings file exists (service_contract.hpp, kAudioWifi): without it nothing here runs, the phone is
// told about no media sink and its sound stays on the car's Bluetooth, as before. With it, both streams are mixed
// (platform/audio_mixer.hpp) and written to one playback PCM of the car (platform/evo_alsa_out.hpp), opened at the
// first sound and closed after a few seconds of silence. Nothing is sent on vMOST/MOST: whether the car lets that PCM
// be heard, and at what volume, is the car's own audio management.
//
// The settings file, one setting per line, each optional:
//   pcm=<name>   the playback PCM, a name of the car's /etc/asound.conf ("default" when absent)
//   gain=<0-100> the whole mix, in percent (100 when absent)
//   duck=<0-100> what is left of media while a voice prompt plays, in percent (30 when absent)
#ifndef AA_ENTRYNAV_PLATFORM_EVO_AUDIO_HPP
#define AA_ENTRYNAV_PLATFORM_EVO_AUDIO_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "audio_mixer.hpp"
#include "evo_alsa_out.hpp"

namespace evo {

struct AudioSettings {
    std::string pcm;
    int gainPercent;
    int duckPercent;
    AudioSettings() : pcm("default"), gainPercent(100), duckPercent(30) {}
};

// A PCM name as accepted from the file: 1 to 63 of A-Z a-z 0-9 _ - . : , =
bool validPcmName(const std::string& name);
// False when there is no file: sound stays on Bluetooth. An invalid line keeps that setting's default.
bool loadAudioSettings(const char* path, AudioSettings& settings);

class EvoAudio {
public:
    EvoAudio();
    ~EvoAudio();

    // False when the car's libasound cannot be loaded or the thread cannot start: sound must stay on Bluetooth.
    bool start(const AudioSettings& settings);
    void stop();

    // From the session thread; none blocks longer than a copy of the sound.
    // A stream starts (config: the index in the channel's list of configurations) or stops.
    void stream(uint8_t channel, bool started, uint32_t config);
    void data(uint8_t channel, const uint8_t* pcm, size_t len);
    void sessionEnded();
    AudioMixer::Stats stats();  // what came in, what was lost, what was played

    // The session's callbacks (core/aa_session.hpp), user = the EvoAudio.
    static void onData(uint8_t channel, const uint8_t* pcm, size_t len, uint64_t pts, void* user);
    static void onStream(uint8_t channel, bool started, uint32_t config, void* user);

private:
    static void* threadStub(void* arg);
    void loop();
    bool prime();
    void logStats(const char* when);

    AudioSettings settings_;
    AlsaOut out_;  // the playback thread only, once started
    pthread_t thread_;
    pthread_mutex_t mu_;
    pthread_cond_t cv_;
    bool started_, quit_;
    AudioMixer mixer_;  // under mu_
};

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_EVO_AUDIO_HPP
