// Copyright (c) 2026 CallMeLewis
// Read-only BlueZ 4 calls to the car's Bluetooth stack (see bluez_adapter.hpp).
#include "bluez_adapter.hpp"

#include <dbus/dbus.h>
#include <pthread.h>

namespace evo {
namespace {

const char* const kService = "org.bluez";
const char* const kAdapterInterface = "org.bluez.Adapter";
const char* const kDeviceInterface = "org.bluez.Device";
const char* const kAdapterPaths[] = {"/org/bluez/hci1", "/org/bluez/hci0"};
const char* const kDoesNotExist = "org.bluez.Error.DoesNotExist";
const int kCallMs = 1000;

pthread_once_t g_once = PTHREAD_ONCE_INIT;
void initThreads() {
    dbus_threads_init_default();
}

// A private system-bus connection for one question, closed when it goes: the bus libdbus shares within the process
// belongs to whoever opened it.
class Bus {
public:
    Bus() : conn_(nullptr) {
        pthread_once(&g_once, initThreads);
        DBusError e;
        dbus_error_init(&e);
        conn_ = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
        dbus_error_free(&e);
        if (conn_) {
            dbus_connection_set_exit_on_disconnect(conn_, FALSE);
        }
    }
    ~Bus() {
        if (conn_) {
            dbus_connection_close(conn_);
            dbus_connection_unref(conn_);
        }
    }
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    bool ok() const { return conn_ != nullptr; }

    // Sends the call (taking it) and returns the reply, or null with `error` set to the error name when there is one.
    DBusMessage* call(DBusMessage* m, std::string& error) {
        error.clear();
        if (!m) {
            return nullptr;
        }
        DBusError e;
        dbus_error_init(&e);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, m, kCallMs, &e);
        dbus_message_unref(m);
        if (dbus_error_is_set(&e)) {
            error = e.name ? e.name : "error";
        }
        dbus_error_free(&e);
        return reply;
    }

private:
    DBusConnection* conn_;
};

// GetProperties on an object: walks its a{sv} and hands each name and its value to `take`. False when the call fails
// or the reply is not a dictionary.
template <typename Take>
bool properties(Bus& bus, const char* path, const char* interface, Take take) {
    std::string error;
    DBusMessage* reply = bus.call(dbus_message_new_method_call(kService, path, interface, "GetProperties"), error);
    if (!reply) {
        return false;
    }
    DBusMessageIter it, dict;
    const bool isDict = dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY &&
                        dbus_message_iter_get_element_type(&it) == DBUS_TYPE_DICT_ENTRY;
    if (isDict) {
        dbus_message_iter_recurse(&it, &dict);
        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry, value;
            dbus_message_iter_recurse(&dict, &entry);
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                const char* name = nullptr;
                dbus_message_iter_get_basic(&entry, &name);
                if (dbus_message_iter_next(&entry) && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                    dbus_message_iter_recurse(&entry, &value);
                    take(name ? name : "", value);
                }
            }
            dbus_message_iter_next(&dict);
        }
    }
    dbus_message_unref(reply);
    return isDict;
}

bool readString(DBusMessageIter& value, std::string& out) {
    if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_STRING) {
        return false;
    }
    const char* s = nullptr;
    dbus_message_iter_get_basic(&value, &s);
    out = s ? s : "";
    return true;
}

bool readBool(DBusMessageIter& value, bool& out) {
    if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_BOOLEAN) {
        return false;
    }
    dbus_bool_t b = FALSE;
    dbus_message_iter_get_basic(&value, &b);
    out = b != FALSE;
    return true;
}

}  // namespace

bool readBluezAdapter(BluezAdapter& out) {
    Bus bus;
    if (!bus.ok()) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kAdapterPaths) / sizeof(kAdapterPaths[0]); ++i) {
        BluezAdapter adapter;
        adapter.path = kAdapterPaths[i];
        const bool answered =
            properties(bus, kAdapterPaths[i], kAdapterInterface, [&](const std::string& name, DBusMessageIter& value) {
                if (name == "Address") {
                    readString(value, adapter.address);
                } else if (name == "Pairable") {
                    readBool(value, adapter.pairable);
                } else if (name == "Discoverable") {
                    readBool(value, adapter.discoverable);
                }
            });
        if (answered && !adapter.address.empty()) {
            out = adapter;
            return true;
        }
    }
    return false;
}

int bluezPaired(const std::string& adapterPath, const std::string& phoneAddress) {
    Bus bus;
    if (!bus.ok() || adapterPath.empty()) {
        return -1;
    }
    std::string address(phoneAddress);  // BlueZ keeps addresses in capitals
    for (size_t i = 0; i < address.size(); ++i) {
        if (address[i] >= 'a' && address[i] <= 'f') {
            address[i] = static_cast<char>(address[i] - 'a' + 'A');
        }
    }
    DBusMessage* find = dbus_message_new_method_call(kService, adapterPath.c_str(), kAdapterInterface, "FindDevice");
    const char* arg = address.c_str();
    if (find && !dbus_message_append_args(find, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID)) {
        dbus_message_unref(find);
        find = nullptr;
    }
    std::string error;
    DBusMessage* reply = bus.call(find, error);
    if (!reply) {
        return error == kDoesNotExist ? 0 : -1;
    }
    const char* device = nullptr;
    std::string devicePath;
    if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID) && device) {
        devicePath = device;
    }
    dbus_message_unref(reply);
    if (devicePath.empty()) {
        return -1;
    }
    bool paired = false;
    const bool answered =
        properties(bus, devicePath.c_str(), kDeviceInterface, [&](const std::string& name, DBusMessageIter& value) {
            if (name == "Paired") {
                readBool(value, paired);
            }
        });
    return answered ? (paired ? 1 : 0) : -1;
}

}  // namespace evo
