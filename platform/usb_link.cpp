// Copyright (c) 2026 CallMeLewis
// Getting Android Auto on a cable (see usb_link.hpp).
#include "usb_link.hpp"

#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace evo {

// ---- the car's bus -------------------------------------------------------------------------------------------------

std::vector<UsbDeviceInfo> LinuxUsbBus::list() {
    return listUsbDevices();
}

UsbIo* LinuxUsbBus::open(const UsbDeviceInfo& device) {
    UsbfsIo* io = new UsbfsIo();
    if (!io->open(device.bus, device.device)) {
        const int saved = errno;
        delete io;
        errno = saved;
        return nullptr;
    }
    return io;
}

std::string LinuxUsbBus::holder(const UsbDeviceInfo& device, const AccessoryInterface& interface) {
    return interfaceDriver(device, interface);
}

void LinuxUsbBus::protect(const UsbDeviceInfo& device) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", device.bus, device.device);
    chmod(path, 0600);  // refused when not root: the session then simply runs unprotected
}

void LinuxUsbBus::sleepMs(unsigned ms) {
    usleep(ms * 1000);
}

uint32_t LinuxUsbBus::nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec) * 1000u + static_cast<uint32_t>(ts.tv_nsec / 1000000);
}

// ---- BMW's media program kept away from phones (usb_link.hpp)
// --------------------------------------------------------

const char kUsbUdevRuleDir[] = "/run/udev/rules.d";
const char kUsbUdevRuleName[] = "99-zz-aa-entrynav-usb.rules";
const char kUsbUdevRule[] =
    "# aa-entrynav, while Android Auto over a cable is on: a phone's USB file is root-only from the moment it "
    "appears,\n"
    "# so that only the Android Auto service can ask it for accessory mode. In memory only (/run).\n"
    "ACTION!=\"add\", GOTO=\"aa_entrynav_usb_end\"\n"
    "SUBSYSTEM!=\"usb\", GOTO=\"aa_entrynav_usb_end\"\n"
    "ENV{DEVTYPE}!=\"usb_device\", GOTO=\"aa_entrynav_usb_end\"\n"
    "ATTR{idVendor}==\"05ac\", GOTO=\"aa_entrynav_usb_end\"\n"
    "ATTR{bDeviceClass}==\"09\", GOTO=\"aa_entrynav_usb_end\"\n"
    "ENV{ID_USB_INTERFACES}==\"*:08????:*|*:03????:*|*:01????:*|*:09????:*\", GOTO=\"aa_entrynav_usb_end\"\n"
    "ENV{ID_USB_INTERFACES}==\"*:ff????:*|*:06????:*\", GROUP:=\"root\", MODE:=\"0600\"\n"
    "LABEL=\"aa_entrynav_usb_end\"\n";

bool installUsbUdevRule(const char* dir, std::string& error) {
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        error = std::string(dir) + ": " + strerror(errno);
        return false;
    }
    const std::string path = std::string(dir) + "/" + kUsbUdevRuleName;
    const std::string temporary = std::string(dir) + "/.aa-entrynav-usb.tmp";  // no .rules suffix: udev skips it
    FILE* f = fopen(temporary.c_str(), "w");
    if (!f) {
        error = temporary + ": " + strerror(errno);
        return false;
    }
    const size_t length = strlen(kUsbUdevRule);
    const bool written = fwrite(kUsbUdevRule, 1, length, f) == length;
    const bool closed = fclose(f) == 0;
    if (!written || !closed || chmod(temporary.c_str(), 0644) != 0 || rename(temporary.c_str(), path.c_str()) != 0) {
        error = path + ": " + strerror(errno);
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

void removeUsbUdevRule(const char* dir) {
    unlink((std::string(dir) + "/" + kUsbUdevRuleName).c_str());
}

// ---- the link ------------------------------------------------------------------------------------------------------

UsbLink::UsbLink(UsbBus& bus, Log log, void* user)
    : bus_(bus),
      log_(log),
      user_(user),
      io_(nullptr),
      interface_(0),
      askedMs_(0),
      asked_(false),
      refusedMs_(0),
      refusedForMs_(kUsbRefusedMs),
      refused_(false),
      carrying_(false),
      touchedMs_(0),
      touched_(false),
      quickFromMs_(0),
      quickWhy_(""),
      quickRetries_(0) {}

UsbLink::~UsbLink() {
    close();
}

void UsbLink::say(const char* format, ...) {
    if (!log_) {
        return;
    }
    char line[320];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    log_(line, user_);
}

void UsbLink::close() {
    bridge_.stop();
    if (io_) {
        io_->release(interface_);
        delete io_;
        io_ = nullptr;
    }
    if (carrying_) {
        // The phone may still be plugged in, in accessory mode with no Android Auto behind it: opening it again would
        // only wait kUsbHelloMs for an answer that does not come. The next connect() resets it and asks again.
        carrying_ = false;
        asked_ = false;
        quickPort_ = carriedPort_;
        quickFromMs_ = bus_.nowMs();
        quickWhy_ = "a session on the cable";
        quickRetries_ = kUsbQuickRetries;
    }
}

void UsbLink::plugged(const std::string& port) {
    if (refused_ && refusedPort_ == port) {
        refused_ = false;
        say("%s: plugged in again: asked at once", port.c_str());
    }
    quickPort_ = port;
    quickFromMs_ = bus_.nowMs();
    quickWhy_ = "it was plugged in";
    quickRetries_ = kUsbQuickRetries;
}

void UsbLink::touched(const std::string& port) {
    touchedPort_ = port;
    touchedMs_ = bus_.nowMs();
    touched_ = true;
}

bool UsbLink::refusedLately(const std::string& port) {
    if (refused_ && bus_.nowMs() - refusedMs_ >= refusedForMs_) {
        refused_ = false;
    }
    return refused_ && refusedPort_ == port;
}

unsigned UsbLink::leaveAlone(const std::string& port, bool soon) {
    const uint32_t now = bus_.nowMs();
    // A phone just plugged in, or that had Android Auto running a moment ago, may not be ready yet; it is not
    // necessarily one set to "charging only": asked again soon.
    const bool quick = soon && quickRetries_ > 0 && port == quickPort_ && now - quickFromMs_ < kUsbRefusedMs;
    if (quick) {
        --quickRetries_;
    }
    refused_ = true;
    refusedPort_ = port;
    refusedMs_ = now;
    refusedForMs_ = quick ? kUsbQuickRetryMs : kUsbRefusedMs;
    return refusedForMs_;
}

bool UsbLink::pick(UsbDeviceInfo& out, bool& accessory, bool* present) {
    const std::vector<UsbDeviceInfo> devices = bus_.list();
    // Ports with a phone now. One that just had a request or a reset is kept while it changes mode: its phone coming
    // back is not a plug-in.
    std::vector<std::string> ports;
    for (size_t i = 0; i < devices.size(); ++i) {
        const UsbDeviceInfo& d = devices[i];
        if (!d.descriptors.empty() && ((isAccessoryId(d.vendor, d.product) && hasAccessoryInterface(d.product)) ||
                                       worthAskingForAccessory(d.vendor, &d.descriptors[0], d.descriptors.size()))) {
            ports.push_back(d.name);
        }
    }
    for (size_t i = 0; i < ports.size(); ++i) {
        if (std::find(ports_.begin(), ports_.end(), ports[i]) == ports_.end()) {
            plugged(ports[i]);
        }
    }
    const bool changing = touched_ && bus_.nowMs() - touchedMs_ < kUsbModeChangeMs;
    if (changing && std::find(ports.begin(), ports.end(), touchedPort_) == ports.end() &&
        std::find(ports_.begin(), ports_.end(), touchedPort_) != ports_.end()) {
        ports.push_back(touchedPort_);
    }
    ports_.swap(ports);
    bool found = false;
    if (present) {
        *present = false;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        const UsbDeviceInfo& d = devices[i];
        if (d.descriptors.empty()) {
            continue;
        }
        if (isAccessoryId(d.vendor, d.product) && hasAccessoryInterface(d.product)) {
            out = d;  // a phone already in accessory mode comes first
            accessory = true;
            if (present) {
                *present = true;
            }
            return true;
        }
        if (worthAskingForAccessory(d.vendor, &d.descriptors[0], d.descriptors.size())) {
            if (present) {
                *present = true;
            }
            if (!found && !refusedLately(d.name)) {
                out = d;
                accessory = false;
                found = true;
            }
        }
    }
    return found;
}

bool UsbLink::phoneWaiting(bool* present) {
    UsbDeviceInfo device;
    bool accessory = false;
    return pick(device, accessory, present);
}

bool UsbLink::awaitChange(const std::string& port, unsigned device, UsbDeviceInfo& out, const std::atomic<bool>& quit) {
    const uint32_t start = bus_.nowMs();
    while (!quit && bus_.nowMs() - start < kUsbModeChangeMs) {
        const std::vector<UsbDeviceInfo> devices = bus_.list();
        for (size_t i = 0; i < devices.size(); ++i) {
            if (devices[i].name == port && devices[i].device != device && !devices[i].descriptors.empty()) {
                out = devices[i];
                return true;
            }
        }
        bus_.sleepMs(5);  // short: the phone must be asked before another program asks it
    }
    return false;
}

int UsbLink::openAccessory(const UsbDeviceInfo& device) {
    AccessoryInterface accessory;
    if (!findAccessoryInterface(&device.descriptors[0], device.descriptors.size(), accessory)) {
        say("%s is in accessory mode without a data interface", device.name.c_str());
        return -1;
    }
    bus_.protect(device);
    UsbIo* io = bus_.open(device);
    if (!io) {
        say("%s: cannot open the accessory (%s)", device.name.c_str(), strerror(errno));
        return -1;
    }
    const std::string holder = bus_.holder(device, accessory);
    if (!holder.empty()) {
        const int rc = io->detach(accessory.number);
        say("%s: interface %u was held by %s: %s", device.name.c_str(), accessory.number, holder.c_str(),
            rc == 0 ? "taken" : strerror(-rc));
    }
    const int rc = io->claim(accessory.number);
    if (rc != 0) {
        say("%s: interface %u not available (%s)", device.name.c_str(), accessory.number, strerror(-rc));
        delete io;
        return -1;
    }
    const int socket = bridge_.start(io, accessory.in, accessory.out);
    if (socket < 0) {
        io->release(accessory.number);
        delete io;
        return -1;
    }
    io_ = io;
    interface_ = accessory.number;
    return socket;
}

bool UsbLink::hello(int socket, const std::atomic<bool>& quit) {
    // Channel 0, plain, one whole message of 6 bytes: VersionRequest 1.6, as the session sends it first.
    static const uint8_t request[] = {0x00, 0x03, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x06};
    if (send(socket, request, sizeof(request), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(request))) {
        return false;
    }
    // The answer: channel 0, a VersionResponse (message 2), 8 bytes from Android Auto 1.7. It is read to its end and no
    // further, by its header's length: a byte more would be taken from what the session reads next, and the session
    // would read every frame after it from the wrong place.
    uint8_t answer[64];
    size_t got = 0, want = 4;
    const uint32_t start = bus_.nowMs();
    while (got < want && !quit && bus_.nowMs() - start < kUsbHelloMs) {
        pollfd fd = {socket, POLLIN, 0};
        if (poll(&fd, 1, 50) <= 0) {
            continue;
        }
        const ssize_t n = recv(socket, answer + got, want - got, MSG_DONTWAIT);
        if (n > 0) {
            got += static_cast<size_t>(n);
        } else if (n == 0) {
            return false;  // the phone left
        }
        if (got == 4 && want == 4) {
            want = 4 + ((static_cast<size_t>(answer[2]) << 8) | answer[3]);
            if (want < 6 || want > sizeof(answer)) {
                say("unexpected answer to the version request: %02x %02x %02x %02x", answer[0], answer[1], answer[2],
                    answer[3]);
                return false;
            }
        }
    }
    return got == want && want >= 6 && answer[0] == 0 && answer[4] == 0 && answer[5] == 2;
}

int UsbLink::connect(const std::atomic<bool>& quit) {
    close();
    for (unsigned attempt = 0; attempt < kUsbAttempts && !quit; ++attempt) {
        UsbDeviceInfo device;
        bool accessory = false;
        if (!pick(device, accessory)) {
            return -1;
        }
        const bool ours = accessory && asked_ && askedPort_ == device.name && bus_.nowMs() - askedMs_ < 15000;
        if (accessory && !ours) {
            // Someone else's accessory mode, or one left from a session that ended: back to the normal mode first.
            UsbIo* io = bus_.open(device);
            if (!io) {
                say("%s: cannot open the phone (%s)", device.name.c_str(), strerror(errno));
                return -1;
            }
            const int rc = io->reset();
            delete io;
            touched(device.name);
            say("%s: in accessory mode, with no Android Auto session behind it (another program's, or one that "
                "ended): port reset %s",
                device.name.c_str(), rc == 0 ? "done" : strerror(-rc));
            UsbDeviceInfo back;
            if (!awaitChange(device.name, device.device, back, quit)) {
                say("%s: the phone did not come back after the reset", device.name.c_str());
                return -1;
            }
            device = back;
            accessory = isAccessoryId(device.vendor, device.product);
            if (accessory) {
                say("%s: still in accessory mode after the reset", device.name.c_str());
                continue;
            }
        }
        if (!accessory) {
            UsbIo* io = bus_.open(device);
            if (!io) {
                say("%s: cannot open the phone (%s)", device.name.c_str(), strerror(errno));
                return -1;
            }
            const int version = accessoryProtocol(*io);
            const bool started = version >= 1 && startAccessory(*io, kAndroidAutoIdentity);
            delete io;
            if (version >= 1) {
                touched(device.name);
            }
            if (version < 1) {
                const unsigned ms = leaveAlone(device.name, false);
                say("%s (%04x:%04x): no accessory protocol: not a phone that does Android Auto; left alone for %u s",
                    device.name.c_str(), device.vendor, device.product, ms / 1000);
                return -1;
            }
            if (!started) {
                // Left alone like any refusal: asked again at once, it would be asked every 250 ms.
                const unsigned ms = leaveAlone(device.name, true);
                say("%s: the phone refused the request for accessory mode; left alone for %u s", device.name.c_str(),
                    ms / 1000);
                return -1;
            }
            asked_ = true;
            askedPort_ = device.name;
            askedMs_ = bus_.nowMs();
            UsbDeviceInfo back;
            if (!awaitChange(device.name, device.device, back, quit)) {
                say("%s: asked for Android Auto (protocol %d), but the phone did not come back", device.name.c_str(),
                    version);
                return -1;
            }
            if (!isAccessoryId(back.vendor, back.product)) {
                const unsigned ms = leaveAlone(device.name, true);
                if (ms < kUsbRefusedMs) {
                    say("%s: the phone came back in its normal mode just after %s: asked again in %u s",
                        device.name.c_str(), quickWhy_, ms / 1000);
                } else {
                    say("%s: the phone came back in its normal mode: it refused accessory mode (its USB setting may "
                        "be \"charging only\"); left alone for %u s",
                        device.name.c_str(), ms / 1000);
                }
                return -1;
            }
            device = back;
        }
        const int socket = openAccessory(device);
        if (socket < 0) {
            asked_ = false;
            continue;
        }
        if (hello(socket, quit)) {
            say("%s: Android Auto answers on the cable (%04x:%04x, round %u)", device.name.c_str(), device.vendor,
                device.product, attempt + 1);
            carrying_ = true;
            carriedPort_ = device.name;
            quickRetries_ = 0;
            return socket;
        }
        say("%s: in accessory mode, but Android Auto does not answer (another program asked first?)",
            device.name.c_str());
        ::close(socket);
        close();
        asked_ = false;  // the next round resets the phone and asks again
    }
    return -1;
}

}  // namespace evo
