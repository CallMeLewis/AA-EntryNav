// Copyright (c) 2026 CallMeLewis
// Getting Android Auto on a cable (platform/usb_link.cpp) against a pretend phone that behaves as the real one did:
// it leaves the bus when asked and comes back as an accessory; asked again while it is one, it stops answering; a port
// reset sends it back to its normal mode; it may refuse. And against a rival that asks the phone first, as BMW's media
// program does on the car.
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <deque>
#include <string>
#include <vector>

#include "usb_link.hpp"

#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            return 1;                                            \
        }                                                        \
    } while (0)

namespace {

const char kAndroidAuto[] = "Android/Android Auto";

struct World {
    enum Mode { kNormal, kAccessory, kGone, kUnplugged };
    World()
        : now(1000),
          mode(kNormal),
          device(2),
          allows(true),
          knowsProtocol(true),
          rivalDelayMs(-1),
          backAt(0),
          backAs(kNormal),
          rivalAt(0),
          rivalPending(false),
          broken(false),
          protectedNode(false),
          resets(0),
          starts(0),
          rivalStarts(0),
          claims(0),
          detaches(0),
          held(false) {
        pthread_mutex_init(&mu, nullptr);
    }
    uint32_t now;
    Mode mode;
    unsigned device;
    bool allows, knowsProtocol;
    int rivalDelayMs;  // -1: no rival. Else it asks this long after the phone shows in its normal mode.
    std::string texts[6], identity;
    uint32_t backAt;
    Mode backAs;
    std::string backIdentity;
    uint32_t rivalAt;
    bool rivalPending, broken, protectedNode;
    int resets, starts, rivalStarts, claims, detaches;
    bool held;  // the rival holds the accessory interface
    pthread_mutex_t mu;

    void becomeNormal() {
        mode = kNormal;
        broken = false;
        held = false;
        if (rivalDelayMs == 0) {
            rivalAsks();  // before anyone else can
        } else if (rivalDelayMs > 0) {
            rivalPending = true;
            rivalAt = now + static_cast<uint32_t>(rivalDelayMs);
        }
    }
    void leave(Mode as, const std::string& who, unsigned afterMs) {
        mode = kGone;
        backAs = as;
        backIdentity = who;
        backAt = now + afterMs;
        rivalPending = false;
    }
    void rivalAsks() {
        ++rivalStarts;
        rivalPending = false;
        leave(kAccessory, "BMW/A4A", 1500);
    }
    void advance(unsigned ms) {
        now += ms;
        if (mode == kGone && now >= backAt) {
            ++device;
            protectedNode = false;  // a new device file, as the system makes it
            if (backAs == kAccessory) {
                mode = kAccessory;
                identity = backIdentity;
                held = identity != kAndroidAuto;  // the rival claims what it asked for
            } else {
                becomeNormal();
            }
        }
        if (rivalPending && mode == kNormal && now >= rivalAt) {
            rivalAsks();
        }
    }
    void start() {
        ++starts;
        if (mode == kNormal) {
            leave(allows ? kAccessory : kNormal, texts[0] + "/" + texts[1], allows ? 1500 : 1200);
        } else if (mode == kAccessory) {
            broken = true;  // seen on the real phone: asked again in accessory mode, it stops answering
        }
    }
    void reset() {
        ++resets;
        if (mode == kAccessory || mode == kNormal) {
            leave(kNormal, "", 1200);
        }
    }
};

class FakeIo : public evo::UsbIo {
public:
    FakeIo(World& world, unsigned device) : w_(world), device_(device), pendingIn_(nullptr) {
        if (pipe(wake_) != 0) {
            abort();
        }
        fcntl(wake_[0], F_SETFL, O_NONBLOCK);
    }
    ~FakeIo() override {
        close(wake_[0]);
        close(wake_[1]);
    }
    bool here() const { return w_.device == device_ && (w_.mode == World::kNormal || w_.mode == World::kAccessory); }

    int control(uint8_t, uint8_t request, uint16_t, uint16_t index, uint8_t* data, uint16_t length, unsigned) override {
        pthread_mutex_lock(&w_.mu);
        int rc = -ENODEV;
        if (here()) {
            if (request == 51) {
                if (w_.knowsProtocol) {
                    data[0] = 2;
                    data[1] = 0;
                    rc = 2;
                } else {
                    rc = -EPIPE;
                }
            } else if (request == 52 && index < 6) {
                w_.texts[index] = reinterpret_cast<const char*>(data);
                rc = length;
            } else if (request == 53) {
                w_.start();
                rc = 0;
            }
        }
        pthread_mutex_unlock(&w_.mu);
        return rc;
    }
    int claim(unsigned) override {
        pthread_mutex_lock(&w_.mu);
        const int rc = !here() ? -ENODEV : w_.held ? -EBUSY : 0;
        if (rc == 0) {
            ++w_.claims;
        }
        pthread_mutex_unlock(&w_.mu);
        return rc;
    }
    int release(unsigned) override { return 0; }
    int detach(unsigned) override {
        pthread_mutex_lock(&w_.mu);
        ++w_.detaches;
        w_.held = false;
        pthread_mutex_unlock(&w_.mu);
        return 0;
    }
    int reset() override {
        pthread_mutex_lock(&w_.mu);
        const int rc = here() ? 0 : -ENODEV;
        if (rc == 0) {
            w_.reset();
        }
        pthread_mutex_unlock(&w_.mu);
        return rc;
    }
    int submit(evo::UsbTransfer& t) override {
        pthread_mutex_lock(&w_.mu);
        if (!here()) {
            pthread_mutex_unlock(&w_.mu);
            return -ENODEV;
        }
        if (t.endpoint & 0x80) {
            pendingIn_ = &t;
            feed();
        } else {
            // Android Auto answers its version request, when it is the app behind the accessory.
            if (t.length == 10 && t.data[5] == 1 && w_.mode == World::kAccessory && w_.identity == kAndroidAuto &&
                !w_.broken) {
                static const char answer[] = {0, 3, 0, 8, 0, 2, 0, 1, 0, 7, 0, 0};
                toHost_.push_back(std::string(answer, sizeof(answer)));
            }
            t.done = t.length;
            t.status = 0;
            finish(&t);
            feed();
        }
        pthread_mutex_unlock(&w_.mu);
        return 0;
    }
    int cancel(evo::UsbTransfer& t) override {
        pthread_mutex_lock(&w_.mu);
        if (pendingIn_ == &t) {
            pendingIn_ = nullptr;
            t.done = 0;
            t.status = -ENOENT;
            finish(&t);
        }
        pthread_mutex_unlock(&w_.mu);
        return 0;
    }
    int reap(evo::UsbTransfer** t) override {
        pthread_mutex_lock(&w_.mu);
        int rc = 0;
        if (!done_.empty()) {
            *t = done_.front();
            done_.pop_front();
            char byte;
            if (read(wake_[0], &byte, 1) < 0) {
            }
            rc = 1;
        } else if (!here()) {
            rc = -ENODEV;
        }
        pthread_mutex_unlock(&w_.mu);
        return rc;
    }
    int waitFd() const override { return wake_[0]; }
    short waitEvents() const override { return POLLIN; }

private:
    void finish(evo::UsbTransfer* t) {
        done_.push_back(t);
        if (write(wake_[1], "x", 1) < 0) {
        }
    }
    void feed() {
        if (!pendingIn_ || toHost_.empty()) {
            return;
        }
        memcpy(pendingIn_->data, toHost_.front().data(), toHost_.front().size());
        pendingIn_->done = static_cast<int>(toHost_.front().size());
        pendingIn_->status = 0;
        finish(pendingIn_);
        pendingIn_ = nullptr;
        toHost_.pop_front();
    }
    World& w_;
    unsigned device_;
    int wake_[2];
    evo::UsbTransfer* pendingIn_;
    std::deque<std::string> toHost_;
    std::deque<evo::UsbTransfer*> done_;
};

class FakeBus : public evo::UsbBus {
public:
    explicit FakeBus(World& world) : w(world) {}
    std::vector<evo::UsbDeviceInfo> list() override {
        std::vector<evo::UsbDeviceInfo> devices;
        pthread_mutex_lock(&w.mu);
        if (w.mode == World::kNormal || w.mode == World::kAccessory) {
            evo::UsbDeviceInfo d;
            d.name = "3-1";
            d.bus = 3;
            d.device = w.device;
            const bool accessory = w.mode == World::kAccessory;
            d.vendor = accessory ? 0x18D1 : 0x04E8;
            d.product = accessory ? 0x2D01 : 0x6860;
            // A device descriptor, a configuration, and one interface: the accessory's, or file transfer.
            static const uint8_t head[27] = {18, 1, 0, 2, 0, 0, 0, 64, 0, 0, 0, 0,    0,  1,
                                             1,  2, 3, 1, 9, 2, 0, 0,  1, 1, 0, 0x80, 250};
            d.descriptors.assign(head, head + 27);
            const uint8_t interface[9] = {9,
                                          4,
                                          0,
                                          0,
                                          2,
                                          static_cast<uint8_t>(accessory ? 0xFF : 0x06),
                                          static_cast<uint8_t>(accessory ? 0xFF : 0x01),
                                          static_cast<uint8_t>(accessory ? 0x00 : 0x01),
                                          0};
            d.descriptors.insert(d.descriptors.end(), interface, interface + 9);
            static const uint8_t endpoints[14] = {7, 5, 0x81, 2, 0, 2, 0, 7, 5, 0x01, 2, 0, 2, 0};
            d.descriptors.insert(d.descriptors.end(), endpoints, endpoints + 14);
            devices.push_back(d);
        }
        pthread_mutex_unlock(&w.mu);
        return devices;
    }
    evo::UsbIo* open(const evo::UsbDeviceInfo& device) override {
        pthread_mutex_lock(&w.mu);
        const bool here = w.device == device.device && (w.mode == World::kNormal || w.mode == World::kAccessory);
        pthread_mutex_unlock(&w.mu);
        if (!here) {
            errno = ENODEV;
            return nullptr;
        }
        return new FakeIo(w, device.device);
    }
    std::string holder(const evo::UsbDeviceInfo&, const evo::AccessoryInterface&) override {
        pthread_mutex_lock(&w.mu);
        const std::string who = w.held ? "usbfs" : "";
        pthread_mutex_unlock(&w.mu);
        return who;
    }
    void protect(const evo::UsbDeviceInfo&) override {
        pthread_mutex_lock(&w.mu);
        w.protectedNode = true;
        pthread_mutex_unlock(&w.mu);
    }
    void sleepMs(unsigned ms) override { step(ms); }
    uint32_t nowMs() override {
        step(100);  // time passes whenever it is looked at: no test waits for real
        pthread_mutex_lock(&w.mu);
        const uint32_t now = w.now;
        pthread_mutex_unlock(&w.mu);
        return now;
    }
    void step(unsigned ms) {
        pthread_mutex_lock(&w.mu);
        w.advance(ms);
        pthread_mutex_unlock(&w.mu);
    }
    World& w;
};

std::vector<std::string> g_lines;
void keep(const char* line, void*) {
    g_lines.push_back(line);
}
bool said(const char* what) {
    for (size_t i = 0; i < g_lines.size(); ++i) {
        if (g_lines[i].find(what) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// What the session does first on its socket: its own version request, answered again.
bool sessionSpeaks(int socket) {
    static const uint8_t request[] = {0x00, 0x03, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x06};
    if (send(socket, request, sizeof(request), 0) != static_cast<ssize_t>(sizeof(request))) {
        return false;
    }
    pollfd fd = {socket, POLLIN, 0};
    uint8_t answer[32];
    return poll(&fd, 1, 2000) > 0 && recv(socket, answer, sizeof(answer), 0) == 12 && answer[5] == 2;
}

}  // namespace

int main() {
    const std::atomic<bool> quit(false);
    {  // a phone in its normal mode: asked once, opened, Android Auto answers; its device file made root-only
        World world;
        FakeBus bus(world);
        evo::UsbLink link(bus, keep, nullptr);
        CHECK(link.phoneWaiting());
        const int socket = link.connect(quit);
        CHECK(socket >= 0 && link.active());
        CHECK(world.starts == 1 && world.resets == 0 && world.claims == 1 && world.detaches == 0 &&
              world.protectedNode);
        CHECK(world.identity == kAndroidAuto && said("Android Auto answers on the cable"));
        CHECK(sessionSpeaks(socket));
        close(socket);  // the session ends: the bridge follows
        for (int i = 0; i < 400 && link.active(); ++i) {
            usleep(5000);
        }
        CHECK(!link.active());
        link.close();
        // Plugged in still, in accessory mode from the session that ended a moment ago: reset and asked again at once,
        // as a fresh start, without opening the accessory first to wait for an answer that does not come.
        g_lines.clear();
        const int again = link.connect(quit);
        CHECK(again >= 0 && world.resets == 1 && world.starts == 2 && sessionSpeaks(again));
        CHECK(said("no Android Auto session behind it") && !said("does not answer"));
        close(again);
        link.close();
    }
    {  // after a session, the phone comes back in its normal mode: asked again soon a few times, then left alone
        World world;
        FakeBus bus(world);
        evo::UsbLink link(bus, keep, nullptr);
        int socket = link.connect(quit);
        CHECK(socket >= 0);
        close(socket);
        for (int i = 0; i < 400 && link.active(); ++i) {
            usleep(5000);
        }
        link.close();
        pthread_mutex_lock(&world.mu);
        world.allows = false;
        pthread_mutex_unlock(&world.mu);
        for (unsigned i = 0; i < evo::kUsbQuickRetries; ++i) {
            g_lines.clear();
            CHECK(link.connect(quit) == -1 && said("just after a session on the cable: asked again in 2 s"));
            bool present = false;
            CHECK(!link.phoneWaiting(&present) && present);  // left alone, but on the cable
            bus.step(evo::kUsbQuickRetryMs + 500);
            CHECK(link.phoneWaiting());
        }
        g_lines.clear();
        CHECK(link.connect(quit) == -1 && said("refused accessory mode") && said("left alone for 30 s"));
        bus.step(evo::kUsbQuickRetryMs + 500);
        CHECK(!link.phoneWaiting());
        bus.step(evo::kUsbRefusedMs);
        pthread_mutex_lock(&world.mu);
        world.allows = true;
        pthread_mutex_unlock(&world.mu);
        socket = link.connect(quit);
        CHECK(socket >= 0 && world.resets == 1 && world.starts == static_cast<int>(evo::kUsbQuickRetries) + 3);
        close(socket);
        link.close();
    }
    {  // the rival asked first: the phone is its accessory. Reset, asked before the rival can, Android Auto answers
        World world;
        world.rivalDelayMs = 600;
        FakeBus bus(world);
        world.rivalAsks();
        bus.step(2000);
        CHECK(world.mode == World::kAccessory && world.identity == "BMW/A4A" && world.held);
        g_lines.clear();
        evo::UsbLink link(bus, keep, nullptr);
        const int socket = link.connect(quit);
        CHECK(socket >= 0 && world.identity == kAndroidAuto);
        CHECK(world.resets == 1 && world.starts == 1 && world.rivalStarts == 1 &&
              said("no Android Auto session behind it"));
        CHECK(sessionSpeaks(socket));
        close(socket);
        link.close();
    }
    {  // a rival that always asks first: three rounds, then given up, no hang; the phone is never asked "on top"
        World world;
        world.rivalDelayMs = 0;
        FakeBus bus(world);
        world.rivalAsks();
        bus.step(2000);
        g_lines.clear();
        evo::UsbLink link(bus, keep, nullptr);
        CHECK(link.connect(quit) == -1 && !link.active());
        CHECK(world.resets == 3 && world.starts == 0 && world.identity == "BMW/A4A");
        CHECK(said("still in accessory mode after the reset"));
    }
    {  // a rival holding the interface of an accessory that is Android Auto's: taken from it
        World world;
        FakeBus bus(world);
        evo::UsbLink link(bus, keep, nullptr);
        pthread_mutex_lock(&world.mu);
        world.texts[0] = "Android";
        world.texts[1] = "Android Auto";
        pthread_mutex_unlock(&world.mu);
        g_lines.clear();
        // The phone answers the request, and the rival grabs the interface the moment the accessory appears.
        struct Grab {
            static void* run(void* w) {
                World* world = static_cast<World*>(w);
                for (int i = 0; i < 4000; ++i) {
                    pthread_mutex_lock(&world->mu);
                    const bool there = world->mode == World::kAccessory;
                    if (there && world->claims == 0) {
                        world->held = true;
                    }
                    pthread_mutex_unlock(&world->mu);
                    if (there) {
                        break;
                    }
                    usleep(200);
                }
                return nullptr;
            }
        };
        pthread_t grabber;
        pthread_create(&grabber, nullptr, Grab::run, &world);
        const int socket = link.connect(quit);
        pthread_join(grabber, nullptr);
        CHECK(socket >= 0);
        CHECK(world.detaches == (said("was held by usbfs") ? 1 : 0));  // taken when it was held
        close(socket);
        link.close();
    }
    {  // a phone that refuses: said once, then left alone for a while, then asked again
        World world;
        world.allows = false;
        FakeBus bus(world);
        g_lines.clear();
        evo::UsbLink link(bus, keep, nullptr);
        CHECK(link.connect(quit) == -1 && world.starts == 1 && said("refused accessory mode"));
        bool present = false;
        CHECK(!link.phoneWaiting(&present) && present);  // not hammered: each request drops it off the bus
        CHECK(link.connect(quit) == -1 && world.starts == 1);
        bus.step(evo::kUsbRefusedMs + 1000);
        CHECK(link.phoneWaiting());
        world.allows = true;
        const int socket = link.connect(quit);
        CHECK(socket >= 0 && world.starts == 2);
        close(socket);
        link.close();
    }
    {  // not a phone that does Android Auto: one question, nothing started
        World world;
        world.knowsProtocol = false;
        FakeBus bus(world);
        g_lines.clear();
        evo::UsbLink link(bus, keep, nullptr);
        CHECK(link.connect(quit) == -1 && world.starts == 0 && said("no accessory protocol") && !link.phoneWaiting());
    }
    {  // nothing plugged in; and unplugged while it was changing mode
        World world;
        world.mode = World::kUnplugged;
        FakeBus bus(world);
        evo::UsbLink link(bus, keep, nullptr);
        bool present = true;
        CHECK(!link.phoneWaiting(&present) && !present && link.connect(quit) == -1);
        World leaving;
        FakeBus bus2(leaving);
        g_lines.clear();
        evo::UsbLink link2(bus2, keep, nullptr);
        leaving.allows = true;
        // The phone goes for good the moment it is asked.
        struct Bus3 : FakeBus {
            explicit Bus3(World& w) : FakeBus(w) {}
            void sleepMs(unsigned ms) override {
                pthread_mutex_lock(&w.mu);
                if (w.mode == World::kGone) {
                    w.mode = World::kUnplugged;
                }
                pthread_mutex_unlock(&w.mu);
                FakeBus::sleepMs(ms);
            }
        } bus3(leaving);
        evo::UsbLink link3(bus3, keep, nullptr);
        CHECK(link3.connect(quit) == -1 && said("did not come back"));
    }
    {  // asked to stop: no new round
        World world;
        FakeBus bus(world);
        const std::atomic<bool> stop(true);
        evo::UsbLink link(bus, keep, nullptr);
        CHECK(link.connect(stop) == -1 && world.starts == 0);
    }
    {  // the udev rule that keeps BMW's media program away from phones: written whole, into a folder made for it
        char base[] = "/tmp/usb-rule-XXXXXX";
        CHECK(mkdtemp(base) != nullptr);
        const std::string dir = std::string(base) + "/rules.d", path = dir + "/" + evo::kUsbUdevRuleName;
        std::string error;
        CHECK(evo::installUsbUdevRule(dir.c_str(), error) && error.empty());
        CHECK(evo::installUsbUdevRule(dir.c_str(), error));  // again, over the first: same file
        FILE* f = fopen(path.c_str(), "r");
        CHECK(f != nullptr);
        char text[2048];
        const size_t n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[n] = 0;
        CHECK(!strcmp(text, evo::kUsbUdevRule));
        // Apple's devices, hubs and storage, keyboard, sound or hub interfaces are left alone; the change is final.
        CHECK(strstr(text, "ATTR{idVendor}==\"05ac\", GOTO") && strstr(text, "ATTR{bDeviceClass}==\"09\", GOTO") &&
              strstr(text, "*:08????:*|*:03????:*|*:01????:*|*:09????:*\", GOTO") &&
              strstr(text, "GROUP:=\"root\", MODE:=\"0600\""));
        CHECK(strcmp(evo::kUsbUdevRuleName, "99-security.rules") > 0);      // read after the car's rule it overrides
        CHECK(access((dir + "/.aa-entrynav-usb.tmp").c_str(), F_OK) != 0);  // nothing half-written left behind
        evo::removeUsbUdevRule(dir.c_str());
        CHECK(access(path.c_str(), F_OK) != 0);
        evo::removeUsbUdevRule(dir.c_str());  // already gone: nothing happens
        CHECK(!evo::installUsbUdevRule((std::string(base) + "/missing/rules.d").c_str(), error) && !error.empty());
        rmdir(dir.c_str());
        rmdir(base);
    }
    puts(
        "PASS usb link: a phone asked once and answering, a fresh start straight after a session, refusals just after "
        "a session retried soon then left alone, a rival that asked first "
        "(reset, asked again), a rival that always wins (given up after 3 rounds), an interface taken back, a phone "
        "that refuses left alone, not a phone, unplugged, stop; the udev rule written whole and removed");
    return 0;
}
