// Copyright (c) 2026 CallMeLewis
// platform/bluez_adapter.cpp against a fake of the car's Bluetooth stack: org.bluez on a private bus standing in for
// the system bus, as BMW's Alps stack serves it (BlueZ 4: Adapter.GetProperties, FindDevice, Device.GetProperties).
// Run by `make test-host` under dbus-run-session, DBUS_SYSTEM_BUS_ADDRESS set to that session's bus.
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bluez_adapter.hpp"

#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            exit(1);                                             \
        }                                                        \
    } while (0)

namespace {

// What the fake stack holds: one adapter at `path`, and the devices it knows with their Paired property.
struct Stack {
    std::string path;
    std::string address;  // empty: GetProperties gives no Address
    bool pairable;
    std::map<std::string, bool> devices;
};

class FakeBluez {
public:
    explicit FakeBluez(const Stack& stack) : stack_(stack), stop_(false), conn_(nullptr) {
        DBusError e;
        dbus_error_init(&e);
        conn_ = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
        CHECK(conn_ != nullptr);
        dbus_connection_set_exit_on_disconnect(conn_, FALSE);
        CHECK(dbus_bus_request_name(conn_, "org.bluez", DBUS_NAME_FLAG_DO_NOT_QUEUE, &e) ==
              DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
        dbus_error_free(&e);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeBluez() {
        stop_ = true;
        thread_.join();
        dbus_connection_close(conn_);
        dbus_connection_unref(conn_);
    }
    std::vector<std::string> asked() {
        std::lock_guard<std::mutex> lock(mutex_);
        return asked_;
    }

private:
    void serve() {
        while (!stop_) {
            dbus_connection_read_write(conn_, 20);
            DBusMessage* m = nullptr;
            while ((m = dbus_connection_pop_message(conn_)) != nullptr) {
                if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
                    DBusMessage* reply = answer(m);
                    dbus_connection_send(conn_, reply, nullptr);
                    dbus_message_unref(reply);
                }
                dbus_message_unref(m);
            }
        }
    }

    static void entry(DBusMessageIter& dict, const char* name, int type, const char* signature, const void* value) {
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &name);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, signature, &v);
        dbus_message_iter_append_basic(&v, type, value);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    }

    DBusMessage* answer(DBusMessage* m) {
        const std::string path = dbus_message_get_path(m) ? dbus_message_get_path(m) : "";
        if (path == stack_.path && dbus_message_is_method_call(m, "org.bluez.Adapter", "GetProperties")) {
            DBusMessage* r = dbus_message_new_method_return(m);
            DBusMessageIter it, dict;
            dbus_message_iter_init_append(r, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
            const char* name = "BMW 12345";
            entry(dict, "Name", DBUS_TYPE_STRING, "s", &name);
            const dbus_uint32_t cls = 0x200408;
            entry(dict, "Class", DBUS_TYPE_UINT32, "u", &cls);
            if (!stack_.address.empty()) {
                const char* address = stack_.address.c_str();
                entry(dict, "Address", DBUS_TYPE_STRING, "s", &address);
            }
            const dbus_bool_t pairable = stack_.pairable ? TRUE : FALSE, discoverable = FALSE;
            entry(dict, "Pairable", DBUS_TYPE_BOOLEAN, "b", &pairable);
            entry(dict, "Discoverable", DBUS_TYPE_BOOLEAN, "b", &discoverable);
            dbus_message_iter_close_container(&it, &dict);
            return r;
        }
        if (path == stack_.path && dbus_message_is_method_call(m, "org.bluez.Adapter", "FindDevice")) {
            const char* address = nullptr;
            if (!dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &address, DBUS_TYPE_INVALID)) {
                return dbus_message_new_error(m, "org.bluez.Error.InvalidArguments", "address");
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                asked_.push_back(address);
            }
            if (!stack_.devices.count(address)) {
                return dbus_message_new_error(m, "org.bluez.Error.DoesNotExist", "Device does not exist");
            }
            std::string device = stack_.path + "/dev_" + address;
            for (size_t i = 0; i < device.size(); ++i) {
                device[i] = device[i] == ':' ? '_' : device[i];
            }
            DBusMessage* r = dbus_message_new_method_return(m);
            const char* p = device.c_str();
            dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
            return r;
        }
        if (path.compare(0, stack_.path.size() + 5, stack_.path + "/dev_") == 0 &&
            dbus_message_is_method_call(m, "org.bluez.Device", "GetProperties")) {
            std::string address = path.substr(stack_.path.size() + 5);
            for (size_t i = 0; i < address.size(); ++i) {
                address[i] = address[i] == '_' ? ':' : address[i];
            }
            DBusMessage* r = dbus_message_new_method_return(m);
            DBusMessageIter it, dict;
            dbus_message_iter_init_append(r, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
            const char* a = address.c_str();
            entry(dict, "Address", DBUS_TYPE_STRING, "s", &a);
            const dbus_bool_t paired = stack_.devices[address] ? TRUE : FALSE;
            entry(dict, "Paired", DBUS_TYPE_BOOLEAN, "b", &paired);
            dbus_message_iter_close_container(&it, &dict);
            return r;
        }
        return dbus_message_new_error(m, "org.freedesktop.DBus.Error.UnknownObject", "no such object");
    }

    Stack stack_;
    std::atomic<bool> stop_;
    DBusConnection* conn_;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> asked_;
};

}  // namespace

int main() {
    if (!getenv("DBUS_SYSTEM_BUS_ADDRESS")) {
        puts("SKIP test-bluez-adapter: run under dbus-run-session with DBUS_SYSTEM_BUS_ADDRESS (make test-host)");
        return 0;
    }
    // No stack on the bus: nothing to read, nothing to ask.
    evo::BluezAdapter none;
    CHECK(!evo::readBluezAdapter(none) && none.address.empty());
    CHECK(evo::bluezPaired("/org/bluez/hci1", "AA:BB:CC:DD:EE:01") == -1);
    {
        // BMW's path: the address and the pairing state; phones known paired, known not paired, unknown.
        Stack stack = {"/org/bluez/hci1", "00:1A:7D:DA:71:13", false, {}};
        stack.devices["AA:BB:CC:DD:EE:01"] = true;
        stack.devices["AA:BB:CC:DD:EE:02"] = false;
        FakeBluez fake(stack);
        evo::BluezAdapter car;
        CHECK(evo::readBluezAdapter(car));
        CHECK(car.path == "/org/bluez/hci1" && car.address == "00:1A:7D:DA:71:13" && !car.pairable &&
              !car.discoverable);
        CHECK(evo::bluezPaired(car.path, "aa:bb:cc:dd:ee:01") == 1);  // asked in capitals, as BlueZ keeps them
        CHECK(evo::bluezPaired(car.path, "AA:BB:CC:DD:EE:02") == 0);
        CHECK(evo::bluezPaired(car.path, "AA:BB:CC:DD:EE:03") == 0);
        CHECK(evo::bluezPaired("/org/bluez/hci9", "AA:BB:CC:DD:EE:01") == -1);  // no such adapter
        CHECK(evo::bluezPaired("", "AA:BB:CC:DD:EE:01") == -1);
        const std::vector<std::string> asked = fake.asked();
        CHECK(asked.size() == 3 && asked[0] == "AA:BB:CC:DD:EE:01");
    }
    {
        // The other path, taking pairings.
        Stack stack = {"/org/bluez/hci0", "00:1A:7D:DA:71:14", true, {}};
        FakeBluez fake(stack);
        evo::BluezAdapter car;
        CHECK(evo::readBluezAdapter(car) && car.path == "/org/bluez/hci0" && car.address == "00:1A:7D:DA:71:14" &&
              car.pairable);
    }
    {
        // An adapter that gives no address is no use.
        Stack stack = {"/org/bluez/hci1", "", true, {}};
        FakeBluez fake(stack);
        evo::BluezAdapter car;
        CHECK(!evo::readBluezAdapter(car));
    }
    puts(
        "PASS car Bluetooth: address and pairing state read (hci1, then hci0), phone paired, known and not paired, "
        "or unknown, no stack");
    return 0;
}
