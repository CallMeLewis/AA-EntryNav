// Copyright (c) 2026 CallMeLewis
// From "a phone is on the cable" to "Android Auto answers on a socket": the steps, their order and what to do when
// someone else got to the phone first.
//
// What a real phone (Samsung, Android 16) and the car showed, and what connect() does about it:
//   - A phone in its normal mode enters accessory mode when asked (usb_aoa.hpp) and comes back on the same port
//     within a few seconds, as Android Auto's accessory.
//   - BMW's media program asks every Android phone first, for its own app link: the phone is then in accessory mode
//     under another identity and Android Auto is not behind it. Asking again in that mode breaks the link instead of
//     changing it; a port reset sends the phone back to its normal mode, where it can be asked again. So: reset, then
//     ask the moment the phone is back, before the other program does.
//   - That program runs as an ordinary user: the phone's device file is made root-only as soon as it is ours, so the
//     session cannot be disturbed. The file lives in memory and is made again, as it was, at the next plug-in.
//   - A phone can refuse (seen on a PC with the phone's USB setting on "charging only"): it comes straight back in
//     its normal mode. It is then left alone for a while: every request makes it drop off the bus for a second.
//   - Whether Android Auto is behind an accessory is only known by asking: its version request is sent and an answer
//     awaited before the session is started. The phone answers that request again when the session sends its own.
//   - A session can end with the phone still plugged in (seen on the car: the Android Auto logo, then iDrive again).
//     The phone stays in accessory mode with nothing behind it, so it is reset and asked again at once. Just after
//     such a session, a phone that comes back in its normal mode has not refused for good: it is asked again a few
//     times, kUsbQuickRetryMs apart, before it is left alone for kUsbRefusedMs.
#ifndef AA_ENTRYNAV_PLATFORM_USB_LINK_HPP
#define AA_ENTRYNAV_PLATFORM_USB_LINK_HPP

#include <stdint.h>

#include <atomic>
#include <string>
#include <vector>

#include "usb_aoa.hpp"
#include "usb_bridge.hpp"
#include "usb_io.hpp"

namespace evo {

// The bus as connect() needs it. The car's is LinuxUsbBus; the PC tests have a pretend phone behind theirs.
class UsbBus {
public:
    virtual ~UsbBus() {}
    virtual std::vector<UsbDeviceInfo> list() = 0;
    virtual UsbIo* open(const UsbDeviceInfo& device) = 0;  // the caller deletes it; null with errno set
    // The driver holding the interface ("usbfs": another program), empty when free.
    virtual std::string holder(const UsbDeviceInfo& device, const AccessoryInterface& interface) = 0;
    virtual void protect(const UsbDeviceInfo& device) = 0;  // its device file: root only
    virtual void sleepMs(unsigned ms) = 0;
    virtual uint32_t nowMs() = 0;
};

class LinuxUsbBus : public UsbBus {
public:
    std::vector<UsbDeviceInfo> list() override;
    UsbIo* open(const UsbDeviceInfo& device) override;
    std::string holder(const UsbDeviceInfo& device, const AccessoryInterface& interface) override;
    void protect(const UsbDeviceInfo& device) override;
    void sleepMs(unsigned ms) override;
    uint32_t nowMs() override;
};

// BMW's media program (vmostAuxInUSB, an ordinary user) asks a phone for its own accessory mode within milliseconds
// of it appearing. Seen on the car on 11 November 2026: after a plug-in it won ten rounds in a row and Android Auto
// took 12 s to come up, or 0.5 s when this service happened to ask first. It opens the phone's USB file through the
// usbdev group, which the car's 99-security.rules gives every USB device. While the cable link is on, this rule in
// udev's runtime folder makes the USB file of a phone-like device root-only as udev creates it, before any program
// hears of the device: only this service, which runs as root, can then ask it. The test is worthAskingForAccessory's:
// not Apple's, not a hub, no storage, keyboard, sound or hub interface, and a vendor-specific or picture-transfer one.
// USB sticks are read through the kernel's block device and iPhones are left out, so they are unaffected; BMW's own
// music from an Android phone over USB is not available while the rule is there. Memory only: udev reads /run, which
// is gone at every restart, and the service removes the rule when it stops.
extern const char kUsbUdevRuleDir[];   // /run/udev/rules.d
extern const char kUsbUdevRuleName[];  // after the car's 99-security.rules, whose group it overrides
extern const char kUsbUdevRule[];
// Writes the rule into dir (created if needed), whole or not at all: udev never reads half a file. False with error.
bool installUsbUdevRule(const char* dir, std::string& error);
void removeUsbUdevRule(const char* dir);

const unsigned kUsbModeChangeMs = 5000;  // a phone changing mode is back within this
const unsigned kUsbHelloMs = 3000;       // Android Auto answers its version request within this
const unsigned kUsbRefusedMs = 30000;    // a phone that refused is left alone this long
const unsigned kUsbAttempts = 3;         // rounds of "reset and ask again" in one connect()
const unsigned kUsbQuickRetries = 3;     // after a session on the cable: refusals that are retried soon
const unsigned kUsbQuickRetryMs = 2000;  // how soon

class UsbLink {
public:
    typedef void (*Log)(const char* line, void* user);
    UsbLink(UsbBus& bus, Log log, void* user);
    ~UsbLink();

    // A phone, or a phone-like device, is on the bus and is not being left alone after a refusal. present: whether
    // one is on the bus at all, left alone or not.
    bool phoneWaiting(bool* present = nullptr);
    // One attempt, a few seconds at most. >= 0: the socket Android Auto answers on, which the caller owns and hands to
    // the session. -1: no phone, a refusal, or the other program won every round; the reason is logged.
    int connect(const std::atomic<bool>& quit);
    bool active() const { return bridge_.running(); }  // the cable still carries the session
    // Ends the bridge and gives the phone back. After a session, the next connect() starts afresh on that phone.
    void close();
    UsbBridge::Stats stats() const { return bridge_.stats(); }

private:
    void say(const char* format, ...) __attribute__((format(printf, 2, 3)));
    bool pick(UsbDeviceInfo& out, bool& accessory, bool* present = nullptr);
    // The phone on port refused: left alone for a while, which is returned (ms).
    unsigned leaveAlone(const std::string& port);
    // Waits for the port's device to change: true with what is there now, false when nothing came back or the service
    // stops (the supervisor kills a service still busy 10 s after a stop).
    bool awaitChange(const std::string& port, unsigned device, UsbDeviceInfo& out, const std::atomic<bool>& quit);
    int openAccessory(const UsbDeviceInfo& device);  // the socket, or -1
    bool hello(int socket, const std::atomic<bool>& quit);
    bool refusedLately(const std::string& port);

    UsbBus& bus_;
    Log log_;
    void* user_;
    UsbIo* io_;  // the accessory, while the bridge runs
    unsigned interface_;
    UsbBridge bridge_;
    std::string askedPort_;  // where accessory mode was asked for Android Auto, and when
    uint32_t askedMs_;
    bool asked_;
    std::string refusedPort_;
    uint32_t refusedMs_, refusedForMs_;
    bool refused_;
    std::string carriedPort_;  // where the session that runs, or ran last, came from
    bool carrying_;            // connect() handed out a socket that close() has not ended yet
    uint32_t endedMs_;         // when the last session on the cable ended
    unsigned quickRetries_;    // refusals on carriedPort_ still retried soon (kUsbQuickRetries)
};

}  // namespace evo

#endif  // AA_ENTRYNAV_PLATFORM_USB_LINK_HPP
