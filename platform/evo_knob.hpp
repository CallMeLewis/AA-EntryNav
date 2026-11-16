// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// iDrive controller, keys and volume (MOST, read through vMOST) to Android Auto.

// Observed on the car: FBlock 0xE5 (the controller) is shadowed by HmiMain3D (instances 03 and 05) and NavStartup
// (01); one pair can be shadowed by dozens of clients. We register as a shadow too (a copy of the controller's Status
// messages): nothing is ever sent to the controller and iDrive keeps receiving everything.
// libvmostclient.so.1 is loaded at run time (platform/vmost_client.hpp) and kept until the process exits.
// Decoding: platform/evo_ccs.hpp (instance 03, functions C46/C47/C4F).
//
// A shadow registration alone receives no message: vmostd.conf sets EnableFunctionBasedFiltering=true, and vmostd
// only forwards a status message (OpType > 8) to a client once bit (FBlock + low byte of the function) & 0xFF of that
// client's filter is set. vmostd sets that bit, before routing, when the client sends a request (OpType <= 8) on that
// function. So we send a plain read (Get, OpType 1) of C46/C47/C4F to E5.03: no Notification.Set/Clear, no
// controller setting changed; iDrive receives the status reply too.
//
// A controller that speaks the older CCS protocol (instance 01, functions C10/C20: platform/evo_ccs.hpp) never answers
// those reads, and its messages stay behind the filter: on a car with iDrive software 202306092158 the volume knob
// came through and the controller not at all. So when no CCS35up status message (C46, C47, C4F) has arrived
// aa::CcsProtocol::kSilenceMs after the reads, the same plain read is sent once for C10 and C20 on E5.01. A car whose
// controller answered on CCS35up gets nothing more. Other E5 messages do not count: an i3 sent E5.01 C30 all along,
// let through by our E5.01 copy and by the filter bit the steering wheel phone button's read sets (0x15 + 0x00 and
// 0xE5 + 0x30 both give bit 0x15), and that once kept the older reads from ever going out.
// At the same moment a copy of the other controller instances known on BMW head units (kCcsFallbackShadows, the
// instances NBT-AAD listens to: https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) is asked for: only instances 03
// and 01 are decoded, but a message from another one is logged raw, which tells where that car's controller is.
// The first protocol heard is kept and the other one's status messages are dropped (aa::CcsProtocol), so a car that
// answers both never counts one turn or key twice.
#ifndef AA_ENTRYNAV_PLATFORM_EVO_KNOB_HPP
#define AA_ENTRYNAV_PLATFORM_EVO_KNOB_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "evo_ccs.hpp"
#include "input_mode.hpp"
#include "vmost_client.hpp"

namespace evo {

// Controller keys as with CarPlay (configureHardKeys, evo_knob.cpp).
void configureHardKeys(aa::CcsDecoder& d);
// Android Auto keys that open one of its apps (AAK_TEL / AAK_NAVIGATION / AAK_MEDIA).
bool isAppKey(uint32_t keycode);
// Android Auto keys that iDrive also takes as its BACK (AAK_BACK, and AAK_DPAD_LEFT from a left tilt).
bool isBackKey(uint32_t keycode);

// CCS35up PushButtonsState (C47/C4F), one byte per key, as HmiMain3D reads it (CDevCCS35up::pushbuttonsToKeycodeAction
// and getButtonActivity): 0 released, 1 pressed, 2/3 long press, 4 finger resting on the touch-sensitive key (iDrive:
// "not pressed"), 7 no information. The core decoder takes any non-zero byte as a press, so merely touching a key
// fired it (observed on the car). Normalised before the decoder: 1/2/3 -> 1, 0/4 -> 0, any other value -> the key's
// previous normalised value. prev holds that state (n <= 8).
void normalizePushButtons(const uint8_t* in, size_t n, uint8_t* prev, uint8_t* out);

// The older CCS protocol's front controller: the reads that open vmostd's filter for it, sent only when the CCS35up
// reads brought no CCS35up status within aa::CcsProtocol::kSilenceMs (aa::CcsProtocol::olderDue).
const uint8_t kCcsFallbackInstance = aa::CcsDecoder::kCcsInstance;
const uint16_t kCcsFallbackFunctions[] = {0xC10, 0xC20};  // RotaryEncoder, PushButton
// Further controller instances copied (never written to) from then on, for the log.
const uint8_t kCcsFallbackShadows[] = {0x02, 0x10, 0x12, 0x14};

// Steering wheel telephone button, ControlElementsMFL 0x15.02 (libhmil6services CPxyControlElementsMFL::init:
// 0x400 TelephoneButton, 0x401 AudioVolumeButton, 0x511 SearchSwitch1, 0x512 PTTButton1, 0x513 SpecialFunction).
// Status = 1 byte: 0 released, 1 pressed, 3 no information. Returns 1 / 0, or -1 otherwise.
const uint16_t kMflTelephoneFkt = 0x400;
int decodeTelephoneButton(const VmostMsgView& m);
// The steering wheel voice button, PTTButton1 0x512, read the same way (seen on the car with tools/evo_mfl.cpp).
const uint16_t kMflVoiceFkt = 0x512;
// How long the voice button is held before Android Auto's assistant starts. HmiMain3D's
// CHmiControlElementsMFLController::onPTTButtonStatus starts a 750 ms timer on a press: a release before it starts
// BMW's voice control ("PTT pressed. Starting Dialog..."), which on this car only says the function is not available;
// after it, BMW's voice control ignores the press ("Ignoring because of long press!"). Both read the same messages,
// so 50 ms above BMW's limit keeps a press from going to both.
const uint32_t kVoiceHoldMs = 800;
int decodeVoiceButton(const VmostMsgView& m);

// Volume buttons, as HmiMain3D's CDevVolumeControl reads them (libhmil6services):
//   CECenterStack       MOST 0x15 instance 0x0C fkt 0xC01 AudioVolumeButtonCS, Status: AudioVolume(0..3), Steps
//   ControlElementsMFL  MOST 0x15 instance 0x02 fkt 0x401 AudioVolumeButton,   Status: UWord, AudioVolume, Steps
// AudioVolume 1 / 2 = one direction each (HMI table 03 02 01), 0 and 3 = nothing. Returns 1 for a volume step.
const uint8_t kVolumeFblock = 0x15, kVolumeCsInstance = 0x0C, kVolumeMflInstance = 0x02;
const uint16_t kVolumeCsFkt = 0xC01, kVolumeMflFkt = 0x401;
int decodeVolumeButton(const VmostMsgView& m, unsigned* steps);

// Volume level, read where HmiMain3D's own volume bar reads it: CHmiVolumeIndicatorService watches CPxyAudioMaster
// instance 0, whose init binds fkt 0xC08 (libhmil6services). Array property: Status = PosX, PosY, then records of
// value, min, max, one byte each. Our copy needs vmostd's filter opened by one Get, as for E5: nothing is ever set on
// the AudioMaster. CVisibleVolumeIndicator::onIncomingMessage (libhmil6services 0x310d3c) reads PosX 0 as the whole
// array, record 0 first, and PosX N as the one record N-1. HmiMain3D's CHmiVolumeIndicatorService reads record 0 as
// the entertainment volume and 1 or 2 as a navigation prompt's or a call's (0x25da250); upstream (Sq00ky #166, NBT
// Evo) has PosX 1 entertainment, 2 navigation prompts, 3 telephone, 6 gong. So the music's level is in bytes 2 to 4
// when PosX is 0 or 1. Any other PosX is another volume: the car reports a prompt's own level when a navigation
// prompt starts, and the pill would show it as the music's.
const uint8_t kAudioMasterFblock = 0x20, kAudioMasterInstance = 0x00;
const uint16_t kVolumeLevelFkt = 0xC08;
const uint8_t kWholeVolumeArray = 0x00, kEntertainmentVolume = 0x01;  // PosX that start with the music's level
inline bool isEntertainmentVolume(uint8_t posX) {
    return posX == kWholeVolumeArray || posX == kEntertainmentVolume;
}
struct VolumeLevel {
    unsigned value, min, max;
};
bool decodeVolumeLevel(const VmostMsgView& m, VolumeLevel& out);

class EvoKnob {
public:
    using Callback = void (*)(const aa::CcsEvent&, void*);
    EvoKnob();
    ~EvoKnob();
    bool start(InputMode mode, Callback cb, void* user);
    void setOwned(bool owned);  // keys are forwarded only while the Android Auto picture shows
    using VolumeCallback = void (*)(unsigned int, void*);
    void setVolumeCallback(VolumeCallback cb, void* user) {
        volCb_ = cb;
        volUser_ = user;
    }
    // Hard key left to iDrive (keycode 0 after configureHardKeys), on key-down: name = MENU, COM, MEDIA, NAV...
    using HardKeyCallback = void (*)(const char*, void*);
    void setHardKeyCallback(HardKeyCallback cb, void* user) {
        keyCb_ = cb;
        keyUser_ = user;
    }
    // A key sent to Android Auto that also makes iDrive change screen (it gets every key), on key-down: NAV, MEDIA,
    // COM and the steering wheel phone open one of Android Auto's apps; BACK and a left tilt are iDrive's BACK too.
    // The caller keeps Android Auto up and brings iDrive back.
    void setAppKeyCallback(HardKeyCallback cb, void* user) {
        appKeyCb_ = cb;
        appKeyUser_ = user;
    }
    // Each volume level the car reports (any source: knob, steering wheel, iDrive). Called on the vMOST thread.
    using VolumeLevelCallback = void (*)(const VolumeLevel&, void*);
    void setVolumeLevelCallback(VolumeLevelCallback cb, void* user) {
        levelCb_ = cb;
        levelUser_ = user;
    }
    // During a phone call (iDrive's call screen) COM and the steering wheel phone button stay with iDrive.
    void setPhoneToIdrive(bool v) { phoneToIdrive_ = v; }
    void stop();
    void summary() const;
    void onMessage(const VmostMsgView& m);  // called from the vMOST callback, on our thread
    // Time-driven keys: the voice button once it has been held kVoiceHoldMs. Every 100 ms from the reading loop.
    void onTick(uint32_t nowMs);
    // PC tests only: decoding/forwarding without vmostd (start() is never called).
    void attachForTest(InputMode mode, Callback cb, void* user) {
        mode_ = mode;
        cb_ = cb;
        user_ = user;
    }
    // PC tests only: what the knob thread does each poll turn while the picture is hidden.
    void pollTurnForTest() {
        if (!owned_) {
            releaseHeldKeys("picture gone");
        }
    }
    // The controller protocol kept so far (knob thread; PC tests read it once the messages are in).
    aa::CcsProtocol::Kind protocol() const { return protocol_.kind(); }

private:
    static void* threadMain(void* self);
    void run();
    bool load();
    InputMode mode_;
    Callback cb_;
    void* user_;
    pthread_t thread_;
    bool started_;
    std::atomic<bool> running_, owned_;
    VmostClient vmost_;
    bool connected_;
    unsigned gets_;
    uint8_t registered_[8];  // kInstances, then kCcsFallbackShadows
    int nRegistered_;
    uint8_t volRegistered_[2];
    int nVolRegistered_;
    VolumeCallback volCb_;
    VolumeLevelCallback levelCb_;
    void* levelUser_;
    bool levelRegistered_;
    unsigned levels_;
    HardKeyCallback keyCb_;
    void* keyUser_;
    void* volUser_;
    HardKeyCallback appKeyCb_;
    void* appKeyUser_;
    std::atomic<bool> phoneToIdrive_;
    bool telDown_;
    bool voiceDown_;
    bool voiceSent_;         // this hold has reached Android Auto
    uint32_t voiceSinceMs_;  // when the voice button went down (monoMs)
    uint8_t pushPrev_[8];
    unsigned volumeSteps_;
    aa::CcsDecoder decoder_;
    aa::CcsProtocol protocol_;  // which controller protocol this car speaks, and when to read the older one
    bool otherProtocolLogged_;  // the other protocol's first dropped message was logged
    void forwardKey(uint32_t keycode, const char* what);
    // Keys whose press reached Android Auto: their release always follows, even once the picture is gone (camera,
    // popup, driving mode, menu), or the phone would keep them down. Knob thread only.
    void releaseHeldKeys(const char* why);
    bool takeHeld(uint32_t keycode);
    uint32_t held_[8];
    int nHeld_;
    unsigned messages_, e5_, events_, sent_, rawLogged_;
};

}  // namespace evo

#endif
