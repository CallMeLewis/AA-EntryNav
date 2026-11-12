// Copyright (c) 2026 CallMeLewis
// evo-usb: tries Android Auto's cable link by hand, one step at a time, on the head unit or a Linux PC. A developer
// tool, not part of a release (make dev-tools); it is copied to /tmp and installs nothing.
//
//   evo-usb list                 the bus: each device, its accessory interface and who holds it     (reads only)
//   evo-usb ask <device>         asks the phone which accessory protocol it knows (request 51)
//   evo-usb start <device> [manufacturer model]
//                                tells the phone it is plugged into Android Auto and starts accessory mode (52, 53).
//                                With a manufacturer and a model: as that accessory instead, to stand in for a
//                                program that claimed the phone first (BMW's does on the car).
//   evo-usb reset <device>       a port reset: the phone starts over in the mode it is in
//   evo-usb link                 everything the service does to get Android Auto on the cable, step by step with its
//                                reasons (platform/usb_link.cpp): the phone found, sent back to its normal mode when
//                                another program asked it first, asked, opened, and Android Auto's answer awaited.
//                                Then the phone is given back.
//   evo-usb bluetooth [phone]    the car's Bluetooth as the service tells a phone on a cable: its address, whether it
//                                takes pairings now, and with a phone's address ("AA:BB:CC:DD:EE:FF"), whether that
//                                phone is paired with the car (reads only)
//   evo-usb hello <device> [--take]
//                                on a phone in accessory mode: takes the interface (--take: also from the program
//                                holding it), sends Android Auto's version request through the bridge and prints the
//                                phone's answer. An answer means Android Auto is speaking on the cable.
// <device> is its place on the bus as `list` prints it, "3-1". Nothing is written to the car's storage.
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bluez_adapter.hpp"
#include "usb_aoa.hpp"
#include "usb_bridge.hpp"
#include "usb_io.hpp"
#include "usb_link.hpp"

namespace {

int usage() {
    fprintf(stderr,
            "Usage: evo-usb list | ask <device> | start <device> [manufacturer model] | reset <device> |\n"
            "       evo-usb hello <device> [--take] | link | bluetooth [phone]\n"
            "       <device>: its place on the bus, as list prints it (3-1)\n");
    return 2;
}

bool find(const std::string& name, evo::UsbDeviceInfo& out) {
    const std::vector<evo::UsbDeviceInfo> devices = evo::listUsbDevices();
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].name == name) {
            out = devices[i];
            return true;
        }
    }
    fprintf(stderr, "no device %s on the bus (evo-usb list)\n", name.c_str());
    return false;
}

bool open(const evo::UsbDeviceInfo& device, evo::UsbfsIo& io) {
    if (!io.open(device.bus, device.device)) {
        fprintf(stderr, "cannot open bus %u device %u: %s\n", device.bus, device.device, strerror(errno));
        return false;
    }
    return true;
}

int list() {
    const std::vector<evo::UsbDeviceInfo> devices = evo::listUsbDevices();
    if (devices.empty()) {
        puts("nothing plugged in");
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        const evo::UsbDeviceInfo& d = devices[i];
        const uint8_t* bytes = d.descriptors.empty() ? nullptr : &d.descriptors[0];
        evo::AccessoryInterface accessory;
        printf("%s  %04x:%04x  bus %u device %u", d.name.c_str(), d.vendor, d.product, d.bus, d.device);
        if (evo::isAccessoryId(d.vendor, d.product)) {
            if (bytes && evo::findAccessoryInterface(bytes, d.descriptors.size(), accessory)) {
                const std::string driver = evo::interfaceDriver(d, accessory);
                printf("  ACCESSORY MODE: interface %u, in 0x%02x, out 0x%02x, held by %s", accessory.number,
                       accessory.in, accessory.out, driver.empty() ? "nobody" : driver.c_str());
            } else {
                printf("  accessory mode without a data interface");
            }
        } else if (bytes && evo::worthAskingForAccessory(d.vendor, bytes, d.descriptors.size())) {
            printf("  looks like a phone: worth asking");
        } else {
            printf("  not asked (hub, storage, keyboard, sound or Apple)");
        }
        putchar('\n');
    }
    return 0;
}

int ask(const evo::UsbDeviceInfo& device) {
    evo::UsbfsIo io;
    if (!open(device, io)) {
        return 1;
    }
    const int version = evo::accessoryProtocol(io);
    if (version < 0) {
        printf("no accessory protocol: %s\n", strerror(-version));
        return 1;
    }
    printf("accessory protocol %d\n", version);
    return 0;
}

int start(const evo::UsbDeviceInfo& device, const evo::AoaIdentity& identity) {
    evo::UsbfsIo io;
    if (!open(device, io)) {
        return 1;
    }
    const int version = evo::accessoryProtocol(io);
    printf("accessory protocol: %d\n", version);
    if (version < 0) {
        return 1;
    }
    const bool ok = evo::startAccessory(io, identity);
    printf("texts of %s / %s and the start request: %s\n", identity.manufacturer, identity.model,
           ok ? "accepted" : "REFUSED");
    return ok ? 0 : 1;
}

int reset(const evo::UsbDeviceInfo& device) {
    evo::UsbfsIo io;
    if (!open(device, io)) {
        return 1;
    }
    const int rc = io.reset();
    printf("port reset: %s\n", rc == 0 ? "done" : strerror(-rc));
    return rc == 0 ? 0 : 1;
}

int hello(const evo::UsbDeviceInfo& device, bool take) {
    evo::AccessoryInterface accessory;
    if (!evo::isAccessoryId(device.vendor, device.product) || device.descriptors.empty() ||
        !evo::findAccessoryInterface(&device.descriptors[0], device.descriptors.size(), accessory)) {
        fprintf(stderr, "%s is not in accessory mode (evo-usb start %s first)\n", device.name.c_str(),
                device.name.c_str());
        return 1;
    }
    evo::UsbfsIo io;
    if (!open(device, io)) {
        return 1;
    }
    const std::string holder = evo::interfaceDriver(device, accessory);
    if (!holder.empty()) {
        printf("interface %u is held by %s\n", accessory.number, holder.c_str());
        if (!take) {
            puts("left alone: add --take to take it from that program");
            return 1;
        }
        const int rc = io.detach(accessory.number);
        printf("taken from it: %s\n", rc == 0 ? "done" : strerror(-rc));
    }
    int rc = io.claim(accessory.number);
    if (rc != 0) {
        printf("cannot take interface %u: %s\n", accessory.number, strerror(-rc));
        return 1;
    }
    int status = 1;
    {
        evo::UsbBridge bridge;
        const int session = bridge.start(&io, accessory.in, accessory.out);
        if (session < 0) {
            puts("the bridge did not start");
            io.release(accessory.number);
            return 1;
        }
        // Channel 0, plain, one whole message of 6 bytes: VersionRequest 1.6, as the service sends it first.
        static const uint8_t request[] = {0x00, 0x03, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x06};
        if (send(session, request, sizeof(request), 0) != static_cast<ssize_t>(sizeof(request))) {
            puts("version request: not sent");
        } else {
            puts("version request sent; waiting 3 s for Android Auto's answer");
            pollfd fd = {session, POLLIN, 0};
            uint8_t answer[64];
            ssize_t n = 0;
            if (poll(&fd, 1, 3000) > 0) {
                n = recv(session, answer, sizeof(answer), 0);
            }
            if (n > 0) {
                printf("answer, %d bytes:", static_cast<int>(n));
                for (ssize_t i = 0; i < n; ++i) {
                    printf(" %02x", answer[i]);
                }
                putchar('\n');
                if (n >= 10 && answer[0] == 0 && answer[4] == 0 && answer[5] == 2) {
                    printf("Android Auto answered: version %u.%u, status %u\n", (answer[6] << 8) | answer[7],
                           (answer[8] << 8) | answer[9], n >= 12 ? (answer[10] << 8) | answer[11] : 0u);
                    status = 0;
                }
            } else {
                puts("no answer: the phone is in accessory mode, but Android Auto is not the app on the other end");
            }
        }
        close(session);
        bridge.stop();
        const evo::UsbBridge::Stats stats = bridge.stats();
        printf("bridge: %llu bytes to the phone, %llu from it, ended with %d\n",
               static_cast<unsigned long long>(stats.toPhone), static_cast<unsigned long long>(stats.fromPhone),
               stats.error);
    }
    io.release(accessory.number);
    return status;
}

void printLine(const char* line, void*) {
    printf("[Usb] %s\n", line);
}

int link() {
    evo::LinuxUsbBus bus;
    evo::UsbLink usb(bus, printLine, nullptr);
    if (!usb.phoneWaiting()) {
        puts("no phone on the bus (evo-usb list)");
        return 1;
    }
    const std::atomic<bool> quit(false);
    const int session = usb.connect(quit);
    if (session < 0) {
        puts("no Android Auto on the cable this time: the lines above say why");
        return 1;
    }
    puts("the service would now start its session on this link; given back here");
    close(session);
    usb.close();
    return 0;
}

int bluetooth(const char* phone) {
    evo::BluezAdapter adapter;
    if (!evo::readBluezAdapter(adapter)) {
        puts("the car's Bluetooth stack did not answer (org.bluez on the system bus)");
        return 1;
    }
    printf("car Bluetooth %s (%s): pairable %s, discoverable %s\n", adapter.address.c_str(), adapter.path.c_str(),
           adapter.pairable ? "yes" : "no", adapter.discoverable ? "yes" : "no");
    if (phone) {
        const int paired = evo::bluezPaired(adapter.path, phone);
        printf("phone %s: %s\n", phone,
               paired == 1   ? "paired with the car"
               : paired == 0 ? "not paired with the car"
                             : "could not ask");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        return usage();
    }
    const std::string command = argv[1];
    if (command == "list" && argc == 2) {
        return list();
    }
    if (command == "link" && argc == 2) {
        return link();
    }
    if (command == "bluetooth" && (argc == 2 || argc == 3)) {
        return bluetooth(argc == 3 ? argv[2] : nullptr);
    }
    if (argc < 3) {
        return usage();
    }
    evo::UsbDeviceInfo device;
    if (!find(argv[2], device)) {
        return 1;
    }
    if (command == "ask" && argc == 3) {
        return ask(device);
    }
    if (command == "start" && argc == 3) {
        return start(device, evo::kAndroidAutoIdentity);
    }
    if (command == "start" && argc == 5) {
        const evo::AoaIdentity other = {argv[3], argv[4], "Stand-in accessory", "1.0", "", "0001"};
        return start(device, other);
    }
    if (command == "reset" && argc == 3) {
        return reset(device);
    }
    if (command == "hello" && (argc == 3 || (argc == 4 && !strcmp(argv[3], "--take")))) {
        return hello(device, argc == 4);
    }
    return usage();
}
