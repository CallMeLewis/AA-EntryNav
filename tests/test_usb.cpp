// Copyright (c) 2026 CallMeLewis
// Android Auto over a cable, without a phone: the accessory requests byte for byte, USB descriptors, the bus's device
// list read from a pretend sysfs, and the socket <-> USB bridge against a pretend phone (order, the empty packet,
// transfers cut short, a busy session, the session leaving, the phone unplugged).
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <deque>
#include <string>
#include <vector>

#include "usb_aoa.hpp"
#include "usb_bridge.hpp"
#include "usb_io.hpp"

#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            return 1;                                            \
        }                                                        \
    } while (0)

namespace {

struct Request {
    uint8_t type, request;
    uint16_t value, index;
    std::string data;
};

// A phone as the bridge and the accessory requests see it.
class FakePhone : public evo::UsbIo {
public:
    FakePhone() : protocol(2), failStringAt(-1), shortOut(0), gone(false), claimed(-1), detached(-1), resets(0) {
        pthread_mutex_init(&mu_, nullptr);
        if (pipe(wake_) != 0) {
            abort();
        }
        fcntl(wake_[0], F_SETFL, O_NONBLOCK);
        pendingIn_ = nullptr;
    }
    ~FakePhone() override {
        close(wake_[0]);
        close(wake_[1]);
        pthread_mutex_destroy(&mu_);
    }

    int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length,
                unsigned) override {
        Request r = {type, request, value, index, std::string()};
        if (!(type & 0x80) && data) {
            r.data.assign(reinterpret_cast<const char*>(data), length);
        }
        requests.push_back(r);
        if (request == 51) {
            if (protocol < 0) {
                return protocol;
            }
            data[0] = static_cast<uint8_t>(protocol);
            data[1] = 0;
            return 2;
        }
        if (request == 52 && static_cast<int>(index) == failStringAt) {
            return -EPIPE;
        }
        return request == 52 ? length : 0;
    }
    int claim(unsigned interface) override {
        claimed = static_cast<int>(interface);
        return 0;
    }
    int release(unsigned) override { return 0; }
    int detach(unsigned interface) override {
        detached = static_cast<int>(interface);
        return 0;
    }
    int reset() override {
        ++resets;
        return 0;
    }
    int submit(evo::UsbTransfer& t) override {
        pthread_mutex_lock(&mu_);
        if (gone) {
            pthread_mutex_unlock(&mu_);
            return -ENODEV;
        }
        if (t.endpoint & 0x80) {
            pendingIn_ = &t;
            feedLocked();
        } else if (holdOut) {
            pendingOut_ = &t;  // taken by nobody: only a cancel finishes it
        } else {
            int n = t.length;
            if (shortOut > 0 && n > shortOut) {
                n = shortOut;  // the transfer is cut short once
                shortOut = 0;
            }
            written.append(reinterpret_cast<const char*>(t.data), n);
            outLengths.push_back(n);
            outZero.push_back(t.zeroPacket);
            t.done = n;
            t.status = 0;
            finishLocked(&t);
        }
        pthread_mutex_unlock(&mu_);
        return 0;
    }
    int cancel(evo::UsbTransfer& t) override {
        pthread_mutex_lock(&mu_);
        if (pendingIn_ == &t) {
            pendingIn_ = nullptr;
            t.done = 0;
            t.status = -ENOENT;
            finishLocked(&t);
            ++cancelled;
        } else if (pendingOut_ == &t) {
            pendingOut_ = nullptr;
            t.done = 0;
            t.status = -ENOENT;
            finishLocked(&t);
            ++cancelledOut;
        }
        pthread_mutex_unlock(&mu_);
        return 0;
    }
    int reap(evo::UsbTransfer** t) override {
        pthread_mutex_lock(&mu_);
        if (gone) {
            pthread_mutex_unlock(&mu_);
            return -ENODEV;
        }
        if (done_.empty()) {
            pthread_mutex_unlock(&mu_);
            return 0;
        }
        *t = done_.front();
        done_.pop_front();
        char byte;
        if (read(wake_[0], &byte, 1) < 0) {
        }
        pthread_mutex_unlock(&mu_);
        return 1;
    }
    int waitFd() const override { return wake_[0]; }
    short waitEvents() const override { return POLLIN; }

    // The phone sends bytes: they come out of the next transfers from the phone, at most `chunk` each.
    void send(const std::string& bytes, size_t chunk) {
        pthread_mutex_lock(&mu_);
        for (size_t at = 0; at < bytes.size(); at += chunk) {
            toHost_.push_back(bytes.substr(at, chunk));
        }
        feedLocked();
        pthread_mutex_unlock(&mu_);
    }
    // True once the bridge waits for the phone's next data.
    bool listening() {
        pthread_mutex_lock(&mu_);
        const bool waiting = pendingIn_ != nullptr;
        pthread_mutex_unlock(&mu_);
        return waiting;
    }
    void unplug() {
        pthread_mutex_lock(&mu_);
        gone = true;
        if (write(wake_[1], "x", 1) < 0) {
        }
        pthread_mutex_unlock(&mu_);
    }
    std::string writtenCopy() {
        pthread_mutex_lock(&mu_);
        const std::string copy = written;
        pthread_mutex_unlock(&mu_);
        return copy;
    }

    int protocol, failStringAt, shortOut;
    bool gone;
    int claimed, detached, resets;
    int cancelled = 0;
    int cancelledOut = 0;
    bool holdOut = false;  // the phone takes no transfer from the car: its Android Auto stopped reading
    std::vector<Request> requests;
    std::string written;
    std::vector<int> outLengths;
    std::vector<bool> outZero;

private:
    void finishLocked(evo::UsbTransfer* t) {
        done_.push_back(t);
        if (write(wake_[1], "x", 1) < 0) {
        }
    }
    void feedLocked() {
        if (!pendingIn_ || toHost_.empty()) {
            return;
        }
        const std::string& chunk = toHost_.front();
        memcpy(pendingIn_->data, chunk.data(), chunk.size());
        pendingIn_->done = static_cast<int>(chunk.size());
        pendingIn_->status = 0;
        finishLocked(pendingIn_);
        pendingIn_ = nullptr;
        toHost_.pop_front();
    }

    pthread_mutex_t mu_;
    int wake_[2];
    evo::UsbTransfer* pendingIn_;
    evo::UsbTransfer* pendingOut_ = nullptr;
    std::deque<std::string> toHost_;
    std::deque<evo::UsbTransfer*> done_;
};

std::string pattern(size_t size, unsigned seed) {
    std::string s(size, 0);
    for (size_t i = 0; i < size; ++i) {
        s[i] = static_cast<char>((i * 31 + seed * 7 + (i >> 8)) & 0xFF);
    }
    return s;
}

// Reads exactly `size` bytes, or what came before the end or `ms` passed.
std::string readAll(int fd, size_t size, int ms) {
    std::string got;
    char buffer[4096];
    while (got.size() < size) {
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, ms) <= 0) {
            break;
        }
        const ssize_t n = recv(fd, buffer, sizeof(buffer) < size - got.size() ? sizeof(buffer) : size - got.size(), 0);
        if (n <= 0) {
            break;
        }
        got.append(buffer, n);
    }
    return got;
}

bool waitFor(bool (*condition)(void*), void* user, int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (condition(user)) {
            return true;
        }
        usleep(5000);
    }
    return condition(user);
}

bool phoneListening(void* phone) {
    return static_cast<FakePhone*>(phone)->listening();
}

bool bridgeStopped(void* bridge) {
    return !static_cast<evo::UsbBridge*>(bridge)->running();
}

void put(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        abort();
    }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

// A device descriptor and one configuration holding the given interfaces, each {class, sub, protocol, endpoints...}.
std::string descriptors(uint16_t vendor, uint16_t product, uint8_t deviceClass,
                        const std::vector<std::vector<uint8_t>>& interfaces) {
    std::string d;
    const uint8_t device[18] = {18,
                                1,
                                0x00,
                                0x02,
                                deviceClass,
                                0,
                                0,
                                64,
                                static_cast<uint8_t>(vendor & 0xFF),
                                static_cast<uint8_t>(vendor >> 8),
                                static_cast<uint8_t>(product & 0xFF),
                                static_cast<uint8_t>(product >> 8),
                                0,
                                1,
                                1,
                                2,
                                3,
                                1};
    d.append(reinterpret_cast<const char*>(device), 18);
    const uint8_t configuration[9] = {9, 2, 0, 0, static_cast<uint8_t>(interfaces.size()), 1, 0, 0x80, 250};
    d.append(reinterpret_cast<const char*>(configuration), 9);
    for (size_t i = 0; i < interfaces.size(); ++i) {
        const std::vector<uint8_t>& in = interfaces[i];
        const uint8_t interface[9] = {
            9, 4, static_cast<uint8_t>(i), 0, static_cast<uint8_t>(in.size() - 3), in[0], in[1], in[2], 0};
        d.append(reinterpret_cast<const char*>(interface), 9);
        for (size_t e = 3; e < in.size(); ++e) {
            const uint8_t endpoint[7] = {7, 5, in[e], 2, 0x00, 0x02, 0};  // bulk, 512 bytes
            d.append(reinterpret_cast<const char*>(endpoint), 7);
        }
    }
    return d;
}

}  // namespace

int main() {
    using evo::AccessoryInterface;
    typedef std::vector<std::vector<uint8_t>> Interfaces;
    const uint8_t* bytes = nullptr;

    // ---- descriptors ----
    // A phone in accessory mode with adb, as the car showed it (18D1:2D01): FF/FF/00 with two bulk endpoints, then adb.
    Interfaces accessory;
    accessory.push_back({0xFF, 0xFF, 0x00, 0x81, 0x01});
    accessory.push_back({0xFF, 0x42, 0x01, 0x82, 0x02});
    std::string d = descriptors(0x18D1, 0x2D01, 0, accessory);
    bytes = reinterpret_cast<const uint8_t*>(d.data());
    AccessoryInterface found = {9, 9, 9, 9};
    CHECK(evo::findAccessoryInterface(bytes, d.size(), found));
    CHECK(found.configuration == 1 && found.number == 0 && found.in == 0x81 && found.out == 0x01);
    CHECK(evo::isAccessoryId(0x18D1, 0x2D00) && evo::isAccessoryId(0x18D1, 0x2D05) &&
          !evo::isAccessoryId(0x18D1, 0x4EE1) && !evo::isAccessoryId(0x04E8, 0x2D00));
    CHECK(evo::hasAccessoryInterface(0x2D01) && !evo::hasAccessoryInterface(0x2D02));
    // adb first, the accessory second: found by its class, not its place.
    Interfaces swapped;
    swapped.push_back({0xFF, 0x42, 0x01, 0x82, 0x02});
    swapped.push_back({0xFF, 0xFF, 0x00, 0x03, 0x83});
    d = descriptors(0x18D1, 0x2D01, 0, swapped);
    CHECK(evo::findAccessoryInterface(reinterpret_cast<const uint8_t*>(d.data()), d.size(), found) &&
          found.number == 1 && found.in == 0x83 && found.out == 0x03);
    // A phone as it is plugged in (file transfer): no accessory interface, but worth asking.
    Interfaces mtp;
    mtp.push_back({0x06, 0x01, 0x01, 0x81, 0x01, 0x82});
    d = descriptors(0x04E8, 0x6860, 0, mtp);
    bytes = reinterpret_cast<const uint8_t*>(d.data());
    CHECK(!evo::findAccessoryInterface(bytes, d.size(), found) &&
          evo::worthAskingForAccessory(0x04E8, bytes, d.size()));
    // Not asked: a USB stick, a hub, a keyboard, an iPhone, a phone that also shows storage, cut-off descriptors.
    Interfaces stick;
    stick.push_back({0x08, 0x06, 0x50, 0x81, 0x02});
    d = descriptors(0x0781, 0x5581, 0, stick);
    CHECK(!evo::worthAskingForAccessory(0x0781, reinterpret_cast<const uint8_t*>(d.data()), d.size()));
    Interfaces hub;
    hub.push_back({0x09, 0x00, 0x00, 0x81});
    d = descriptors(0x0424, 0x2514, 9, hub);
    CHECK(!evo::worthAskingForAccessory(0x0424, reinterpret_cast<const uint8_t*>(d.data()), d.size()));
    Interfaces keyboard;
    keyboard.push_back({0x03, 0x01, 0x01, 0x81});
    d = descriptors(0x046D, 0xC31C, 0, keyboard);
    CHECK(!evo::worthAskingForAccessory(0x046D, reinterpret_cast<const uint8_t*>(d.data()), d.size()));
    d = descriptors(0x05AC, 0x12A8, 0, mtp);
    CHECK(!evo::worthAskingForAccessory(0x05AC, reinterpret_cast<const uint8_t*>(d.data()), d.size()));
    Interfaces both = mtp;
    both.push_back({0x08, 0x06, 0x50, 0x83, 0x03});
    d = descriptors(0x04E8, 0x6860, 0, both);
    CHECK(!evo::worthAskingForAccessory(0x04E8, reinterpret_cast<const uint8_t*>(d.data()), d.size()));
    d = descriptors(0x18D1, 0x2D01, 0, accessory);
    for (size_t cut = 0; cut < d.size(); ++cut) {  // never reads past the end, whatever is missing
        evo::findAccessoryInterface(reinterpret_cast<const uint8_t*>(d.data()), cut, found);
        evo::worthAskingForAccessory(0x18D1, reinterpret_cast<const uint8_t*>(d.data()), cut);
    }
    std::string broken = d;
    broken[18] = 0;  // a descriptor of length 0 must not loop for ever
    CHECK(!evo::findAccessoryInterface(reinterpret_cast<const uint8_t*>(broken.data()), broken.size(), found));

    // ---- accessory requests ----
    {
        FakePhone phone;
        CHECK(evo::accessoryProtocol(phone) == 2);
        CHECK(phone.requests.size() == 1 && phone.requests[0].type == 0xC0 && phone.requests[0].request == 51);
        CHECK(evo::startAccessory(phone, evo::kAndroidAutoIdentity));
        CHECK(phone.requests.size() == 8);
        const char* const texts[6] = {
            "Android",     "Android Auto", "Android Auto", "2.0.1", "https://developer.android.com/auto/index.html",
            "HU-AAAAAA001"};
        for (int i = 0; i < 6; ++i) {
            const Request& r = phone.requests[1 + i];
            CHECK(r.type == 0x40 && r.request == 52 && r.value == 0 && r.index == i);
            CHECK(r.data == std::string(texts[i]) + std::string(1, '\0'));  // with its final zero
        }
        CHECK(phone.requests[7].type == 0x40 && phone.requests[7].request == 53 && phone.requests[7].data.empty());
    }
    {
        FakePhone phone;  // not a phone that knows the protocol: nothing else is sent
        phone.protocol = -EPIPE;
        CHECK(evo::accessoryProtocol(phone) == -EPIPE);
        FakePhone refusing;  // a text refused: no start
        refusing.failStringAt = 2;
        CHECK(!evo::startAccessory(refusing, evo::kAndroidAutoIdentity));
        CHECK(refusing.requests.size() == 3 && refusing.requests.back().request == 52);
    }

    // ---- the bus's device list ----
    char dir[] = "/tmp/entry-usb-test-XXXXXX";
    CHECK(mkdtemp(dir));
    const std::string root = dir;
    {
        d = descriptors(0x18D1, 0x2D01, 0, accessory);
        mkdir((root + "/3-1").c_str(), 0755);
        put(root + "/3-1/idVendor", "18d1\n");
        put(root + "/3-1/idProduct", "2d01\n");
        put(root + "/3-1/busnum", "3\n");
        put(root + "/3-1/devnum", "4\n");
        put(root + "/3-1/descriptors", d);
        mkdir((root + "/usb3").c_str(), 0755);  // a root hub: not listed
        put(root + "/usb3/idVendor", "1d6b\n");
        mkdir((root + "/3-1:1.0").c_str(), 0755);  // an interface: not listed
        mkdir((root + "/1-1.2").c_str(), 0755);    // unplugged while it was read: no descriptors, not listed
        put(root + "/1-1.2/idVendor", "04e8\n");
        mkdir((root + "/drivers").c_str(), 0755);
        mkdir((root + "/drivers/usbfs").c_str(), 0755);
        CHECK(symlink((root + "/drivers/usbfs").c_str(), (root + "/3-1:1.0/driver").c_str()) == 0);
        std::vector<evo::UsbDeviceInfo> list = evo::listUsbDevices(root.c_str());
        CHECK(list.size() == 1 && list[0].name == "3-1" && list[0].vendor == 0x18D1 && list[0].product == 0x2D01 &&
              list[0].bus == 3 && list[0].device == 4 && list[0].descriptors.size() == d.size());
        CHECK(evo::findAccessoryInterface(&list[0].descriptors[0], list[0].descriptors.size(), found));
        CHECK(evo::interfaceDriver(list[0], found, root.c_str()) == "usbfs");  // a program holds it, as on the car
        AccessoryInterface other = found;
        other.number = 1;
        CHECK(evo::interfaceDriver(list[0], other, root.c_str()).empty());
        CHECK(evo::listUsbDevices((root + "/nowhere").c_str()).empty());
    }
    if (system((std::string("rm -rf ") + root).c_str()) != 0) {
        return 1;
    }

    // ---- the bridge ----
    {  // both ways, in order; the empty packet asked for; a transfer cut short is finished
        FakePhone phone;
        phone.shortOut = 100;
        evo::UsbBridge bridge;
        const int session = bridge.start(&phone, 0x81, 0x01);
        CHECK(session >= 0 && bridge.running());
        const std::string up = pattern(1024, 1);  // fills its last packet exactly
        CHECK(send(session, up.data(), up.size(), 0) == static_cast<ssize_t>(up.size()));
        const std::string down = pattern(50000, 2);
        phone.send(down, 16384);
        CHECK(readAll(session, down.size(), 2000) == down);
        for (int waited = 0; waited < 2000 && phone.writtenCopy().size() < up.size(); waited += 5) {
            usleep(5000);
        }
        CHECK(phone.writtenCopy() == up);
        CHECK(phone.outLengths.size() == 2 && phone.outLengths[0] == 100 && phone.outLengths[1] == 924);
        CHECK(phone.outZero[0] && phone.outZero[1]);
        const evo::UsbBridge::Stats stats = bridge.stats();
        CHECK(stats.fromPhone == down.size() && stats.toPhone == up.size() && stats.transfersIn == 4);
        // The session leaves: the bridge ends by itself, and takes back the transfer it had queued.
        close(session);
        CHECK(waitFor(bridgeStopped, &bridge, 2000));
        CHECK(bridge.stats().error == 0 && phone.cancelled == 1);
        bridge.stop();
    }
    {  // a session that reads nothing for a while and writes meanwhile: neither side waits for the other
        FakePhone phone;
        evo::UsbBridge bridge;
        const int session = bridge.start(&phone, 0x81, 0x01);
        CHECK(session >= 0);
        const std::string down = pattern(900000, 3);  // more than the socket holds
        phone.send(down, 16384);
        usleep(100000);
        std::string up;
        for (int i = 0; i < 40; ++i) {
            const std::string part = pattern(3000 + i, 10 + i);
            CHECK(send(session, part.data(), part.size(), 0) == static_cast<ssize_t>(part.size()));
            up += part;
        }
        for (int waited = 0; waited < 3000 && phone.writtenCopy().size() < up.size(); waited += 5) {
            usleep(5000);
        }
        CHECK(phone.writtenCopy() == up);                    // written although nothing was read yet
        CHECK(readAll(session, down.size(), 3000) == down);  // and everything from the phone, in order
        bridge.stop();                                       // stopped from outside: the session reads the end
        char byte;
        CHECK(recv(session, &byte, 1, 0) == 0);
        close(session);
    }
    {  // the phone stops taking data, then the session gives up: the bridge ends anyway and frees the cable
        FakePhone phone;
        phone.holdOut = true;
        evo::UsbBridge bridge;
        const int session = bridge.start(&phone, 0x81, 0x01);
        CHECK(session >= 0);
        CHECK(send(session, "ping", 4, 0) == 4);  // queued to the phone, which never takes it
        usleep(50000);
        CHECK(bridge.running());
        close(session);
        CHECK(waitFor(bridgeStopped, &bridge, 2000));
        CHECK(bridge.stats().error == 0 && phone.cancelledOut == 1 && phone.cancelled == 1);
        bridge.stop();
    }
    {  // the phone is unplugged: the session reads the end of its socket
        FakePhone phone;
        evo::UsbBridge bridge;
        const int session = bridge.start(&phone, 0x81, 0x01);
        CHECK(session >= 0);
        phone.send("hello", 16384);
        CHECK(readAll(session, 5, 2000) == "hello");
        // Unplugged while the bridge waits for data, as a phone left alone is. Sooner, the bridge may be asking for
        // that data at this instant and report the refusal as an I/O error instead.
        CHECK(waitFor(phoneListening, &phone, 2000));
        phone.unplug();
        CHECK(waitFor(bridgeStopped, &bridge, 2000));
        char byte;
        CHECK(recv(session, &byte, 1, 0) == 0 && bridge.stats().error == -ENODEV);
        close(session);
    }
    puts(
        "PASS usb: accessory requests byte for byte, descriptors (accessory with adb, file transfer phone, stick, hub, "
        "keyboard, iPhone, cut short), device list from sysfs, bridge both ways in order with the empty packet, a "
        "transfer cut short, a busy session, the session leaving, the session leaving a phone that takes nothing, the "
        "phone unplugged");
    return 0;
}
