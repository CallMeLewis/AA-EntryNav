// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Captures the car microphone through the HU's libasound, on a thread woken by the phone's mic requests.
#include "evo_mic.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>

#include "evo_log.hpp"

namespace evo {

namespace {
const int kStreamCapture = 1;                 // SND_PCM_STREAM_CAPTURE
const int kFormatS16LE = 2;                   // SND_PCM_FORMAT_S16_LE
const int kAccessRwInterleaved = 3;           // SND_PCM_ACCESS_RW_INTERLEAVED
const unsigned kRate = 16000, kFrames = 320;  // 20 ms per read
const char* const kPcm = "mic1";              // the car's microphone (observed working on the car)
const int kNonblock = 1;                      // SND_PCM_NONBLOCK: a read never waits for ever, so stop() returns
const int kWaitMs = 100;                      // one wait for data, then want_ and quit_ are looked at again
const uint32_t kSilentMs = 2000;              // open, but no sound for this long: the capture is stuck
const uint32_t kReopenMs = 1000;              // after a failure, the next open waits this long
const unsigned kFailedOpens = 3;              // failures in a row before waiting for the phone's next request
const long kAgain = -11;                      // -EAGAIN: no data yet (non-blocking)
}  // namespace

EvoMic::EvoMic()
    : cb_(nullptr),
      user_(nullptr),
      lib_(nullptr),
      started_(false),
      quit_(false),
      want_(false),
      open_(nullptr),
      setParams_(nullptr),
      readi_(nullptr),
      recover_(nullptr),
      wait_(nullptr),
      close_(nullptr),
      strerror_(nullptr) {
    pthread_mutex_init(&mu_, nullptr);
    pthread_cond_init(&cv_, nullptr);
}

EvoMic::~EvoMic() {
    stop();
    if (lib_) {
        dlclose(lib_);
    }
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
}

bool EvoMic::load() {
    lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        ELOG("Mic", "libasound.so.2 not found: no microphone");
        return false;
    }
    *reinterpret_cast<void**>(&open_) = dlsym(lib_, "snd_pcm_open");
    *reinterpret_cast<void**>(&setParams_) = dlsym(lib_, "snd_pcm_set_params");
    *reinterpret_cast<void**>(&readi_) = dlsym(lib_, "snd_pcm_readi");
    *reinterpret_cast<void**>(&recover_) = dlsym(lib_, "snd_pcm_recover");
    *reinterpret_cast<void**>(&wait_) = dlsym(lib_, "snd_pcm_wait");
    *reinterpret_cast<void**>(&close_) = dlsym(lib_, "snd_pcm_close");
    *reinterpret_cast<void**>(&strerror_) = dlsym(lib_, "snd_strerror");
    if (!open_ || !setParams_ || !readi_ || !recover_ || !wait_ || !close_ || !strerror_) {
        ELOG("Mic", "libasound incomplete: no microphone");
        dlclose(lib_);
        lib_ = nullptr;
        return false;
    }
    return true;
}

bool EvoMic::start(DataCallback cb, void* user) {
    if (started_) {
        return true;
    }
    if (!lib_ && !load()) {
        return false;
    }
    cb_ = cb;
    user_ = user;
    quit_ = false;
    want_ = false;
    if (pthread_create(&thread_, nullptr, threadStub, this) != 0) {
        ELOG("Mic", "cannot start the thread");
        return false;
    }
    started_ = true;
    ELOG("Mic", "ready (%s, 16 kHz mono)", kPcm);
    return true;
}

void EvoMic::request(bool open) {
    pthread_mutex_lock(&mu_);
    want_ = open;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
}

void EvoMic::stop() {
    if (!started_) {
        return;
    }
    pthread_mutex_lock(&mu_);
    quit_ = true;
    want_ = false;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
    pthread_join(thread_, nullptr);
    started_ = false;
}

void* EvoMic::threadStub(void* arg) {
    static_cast<EvoMic*>(arg)->loop();
    return nullptr;
}

// The phone still wants the microphone and the service is not stopping.
bool EvoMic::wanted() {
    pthread_mutex_lock(&mu_);
    const bool keep = want_ && !quit_;
    pthread_mutex_unlock(&mu_);
    return keep;
}

// After a failure: waits while the phone still wants the microphone, until it closes it or the service stops.
void EvoMic::waitForNextRequest() {
    pthread_mutex_lock(&mu_);
    while (!quit_ && want_) {
        pthread_cond_wait(&cv_, &mu_);
    }
    pthread_mutex_unlock(&mu_);
}

void EvoMic::loop() {
    int16_t buf[kFrames];
    unsigned failedRuns = 0;  // opens in a row that ended in a failure, without any sound
    for (;;) {
        pthread_mutex_lock(&mu_);
        while (!quit_ && !want_) {
            pthread_cond_wait(&cv_, &mu_);
        }
        const bool quit = quit_;
        pthread_mutex_unlock(&mu_);
        if (quit) {
            return;
        }

        void* h = nullptr;
        int err = open_(&h, kPcm, kStreamCapture, kNonblock);
        if (err >= 0) {
            err = setParams_(h, kFormatS16LE, kAccessRwInterleaved, 1, kRate, 1, 100000);
        }
        if (err < 0) {
            ELOG("Mic", "cannot open %s: %s", kPcm, strerror_(err));
            if (h) {
                close_(h);
            }
            waitForNextRequest();  // instead of retrying in a loop
            continue;
        }
        ELOG("Mic", "open for the phone (%s)", kPcm);
        unsigned long frames = 0;
        unsigned errors = 0;
        bool failed = false;
        uint32_t lastSoundMs = monoMs();
        while (wanted()) {
            // Never blocks for long: a capture that stops producing must not hold stop() for ever.
            const int ready = wait_(h, kWaitMs);
            long n = ready < 0 ? ready : ready == 0 ? kAgain : readi_(h, buf, kFrames);
            if (n == kAgain) {
                if (monoMs() - lastSoundMs >= kSilentMs) {
                    ELOG("Mic", "no sound from %s for %u ms: closed", kPcm, kSilentMs);
                    failed = true;
                    break;
                }
                continue;
            }
            if (n < 0) {
                if (++errors <= 5) {
                    ELOG("Mic", "read: %s", strerror_(static_cast<int>(n)));
                }
                if (recover_(h, static_cast<int>(n), 1) < 0) {
                    failed = true;
                    break;
                }
                continue;
            }
            lastSoundMs = monoMs();
            frames += static_cast<unsigned long>(n);
            if (cb_ && n > 0) {
                cb_(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n) * 2, user_);
            }
        }
        close_(h);
        ELOG("Mic", "closed (%lu ms captured, %u error(s))", frames / 16, errors);
        failedRuns = !failed ? 0 : frames ? 1 : failedRuns + 1;
        if (!failed) {
            continue;
        }
        // Not reopened at once: a capture that fails again straight away would spin, open and close, for as long as
        // the Assistant listens. A pause, and after a few failures in a row, nothing until the phone asks again.
        if (failedRuns >= kFailedOpens) {
            ELOG("Mic", "%u failures in a row: closed until the phone asks for the microphone again", failedRuns);
            waitForNextRequest();
            failedRuns = 0;
            continue;
        }
        for (uint32_t since = monoMs(); wanted() && monoMs() - since < kReopenMs;) {
            usleep(50000);
        }
    }
}

}  // namespace evo
