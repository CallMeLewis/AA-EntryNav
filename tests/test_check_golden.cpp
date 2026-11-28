// Copyright (c) 2026 CallMeLewis
// The C++ evo-check's outputs, recorded for its Rust port (rust/evo-check) and checked against what was recorded:
//   evo_check/dump.golden        --dbus-watch's dump of the arguments of each message of dbus/messages.bin
//   evo_check/alps-config.golden the summary of each alps-config-*.xml case (--context reads
//                                /etc/alps/evolution/evo_config.xml), and of a file past the size limit
//   evo_check/usage.golden       evo-check's usage text
//   evo_check/alps/owner.txt     the unique name of the fake Alps service below
//   evo_check/alps/requests.bin  what evo-check's Alps link (platform/alps_spp.cpp, monitor mode) sends the service
//                                for --bluetooth: Introspect and two configuration reads, each with serial 0 and
//                                without the sender the bus adds
//   evo_check/alps/signals.bin   the signals the service sent, as the bus delivered them to the link
//   evo_check/alps/monitor.golden the link's log lines for them, without each line's time
// rust/evo-check/tests/golden.rs reads the same files and must produce the same bytes.
//   test-check-golden DIR            check that the C++ still gives what DIR (tests/fixtures) holds (make test-host)
//   test-check-golden DIR --record   write them again (only when the C++'s behaviour is meant to change)
// Run under dbus-run-session, with DBUS_SYSTEM_BUS_ADDRESS the session bus: the fake Alps service owns its name there.
#include <dbus/dbus.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "alps_spp.hpp"
#include "evo_log.hpp"
#include "evo_process.hpp"

// evo-check reads the Alps configuration from a fixed path; here it reads the case being recorded.
static const char* g_alpsConfig = nullptr;
static int goldenOpen(const char* path, int flags) {
    if (g_alpsConfig && !strcmp(path, "/etc/alps/evolution/evo_config.xml")) {
        path = g_alpsConfig;
    }
    return open(path, flags);
}
#define open goldenOpen
#define main evo_check_main
#include "../tools/evo_check.cpp"
#undef main
#undef open

#include <atomic>
#include <fstream>
#include <iterator>
#include <thread>

namespace {

typedef std::vector<uint8_t> Bytes;

void need(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL check golden: %s\n", what);
        exit(1);
    }
}

std::string readFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// What f prints on stdout.
template <typename F>
std::string captured(F f) {
    fflush(stdout);
    FILE* capture = tmpfile();
    need(capture != nullptr, "a temporary file");
    const int saved = dup(STDOUT_FILENO);
    dup2(fileno(capture), STDOUT_FILENO);
    f();
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    rewind(capture);
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), capture)) > 0) {
        s.append(buf, n);
    }
    fclose(capture);
    return s;
}

std::string dumpAll(const std::string& wire) {
    return captured([&] {
        size_t at = 0;
        for (unsigned i = 0; at < wire.size(); ++i) {
            const int n = dbus_message_demarshal_bytes_needed(wire.data() + at, static_cast<int>(wire.size() - at));
            need(n > 0, "a whole message in the corpus");
            DBusError e;
            dbus_error_init(&e);
            DBusMessage* m = dbus_message_demarshal(wire.data() + at, n, &e);
            need(m != nullptr, "a valid message in the corpus");
            printf("# message %u\n", i);
            DBusMessageIter it;
            if (dbus_message_iter_init(m, &it)) {
                dbusDump(&it, 0);
            }
            dbus_message_unref(m);
            at += static_cast<size_t>(n);
        }
    });
}

std::string alpsConfigAll(const std::string& dir) {
    std::string out;
    for (int i = 1;; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "alps-config-%d.xml", i);
        const std::string path = dir + "/evo_check/" + name;
        if (access(path.c_str(), R_OK) != 0) {
            break;
        }
        g_alpsConfig = path.c_str();
        out += "# " + std::string(name) + "\n" + captured([] { alpsConfigSummary(); });
    }
    // Past readText's limit of 262144 bytes, and a folder: both unreadable.
    char big[] = "/tmp/evo-check-big-XXXXXX";
    const int fd = mkstemp(big);
    need(fd >= 0, "a temporary file");
    const std::string filler(262145, 'x');
    need(write(fd, filler.data(), filler.size()) == static_cast<ssize_t>(filler.size()), "write the big case");
    close(fd);
    g_alpsConfig = big;
    out += "# 262145 bytes\n" + captured([] { alpsConfigSummary(); });
    unlink(big);
    g_alpsConfig = "/tmp";
    out += "# a folder\n" + captured([] { alpsConfigSummary(); });
    g_alpsConfig = "/nonexistent/evo_config.xml";
    out += "# missing\n" + captured([] { alpsConfigSummary(); });
    g_alpsConfig = nullptr;
    return out;
}

// ---- a fake Alps service, and the link watching it ----------------------------------------------------------------

const char* const kAlpsPath = "/org/alps/evolution/hli";
const char* const kSpp = "org.alps.evo.SPP";
const char* const kDm = "org.alps.evo.DM";
const char* const kXml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node><interface name=\"org.alps.evo.SPP\"><signal name=\"BtApplSppDataInd\"/></interface></node>";

struct FakeAlps {
    DBusConnection* conn;
    std::atomic<bool> running;
    std::thread worker;
    Bytes requests;
    std::string owner;
    FakeAlps() : conn(nullptr), running(true) {
        DBusError e;
        dbus_error_init(&e);
        conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
        need(conn != nullptr, "the fake Alps service's bus");
        need(dbus_bus_request_name(conn, "org.alps.evolution", DBUS_NAME_FLAG_DO_NOT_QUEUE, &e) ==
                 DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER,
             "the name org.alps.evolution");
        owner = dbus_bus_get_unique_name(conn);
        worker = std::thread([this] {
            while (running) {
                dbus_connection_read_write(conn, 20);
                DBusMessage* m;
                while ((m = dbus_connection_pop_message(conn)) != nullptr) {
                    serve(m);
                    dbus_message_unref(m);
                }
            }
        });
    }
    // The worker gives the connection back, for a blocking call from this thread.
    void stopServing() {
        running = false;
        if (worker.joinable()) {
            worker.join();
        }
    }
    ~FakeAlps() {
        stopServing();
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
    }
    // The request as the link sent it: serial 0, and without the sender the bus adds.
    void record(DBusMessage* m) {
        DBusMessage* copy = dbus_message_copy(m);
        need(copy != nullptr && dbus_message_set_sender(copy, nullptr), "a copy of the request");
        char* bytes = nullptr;
        int n = 0;
        need(dbus_message_marshal(copy, &bytes, &n), "marshal the request");
        requests.insert(requests.end(), bytes, bytes + n);
        dbus_free(bytes);
        dbus_message_unref(copy);
    }
    void serve(DBusMessage* m) {
        if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
            return;
        }
        if (dbus_message_is_method_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
            record(m);
            DBusMessage* r = dbus_message_new_method_return(m);
            dbus_message_append_args(r, DBUS_TYPE_STRING, &kXml, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, r, nullptr);
            dbus_message_unref(r);
        } else if (dbus_message_is_method_call(m, kDm, "BtApplReadConfigurationReq")) {
            record(m);
            unsigned char instance = 0;
            dbus_uint16_t id = 0;
            dbus_message_get_args(m, nullptr, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &id, DBUS_TYPE_INVALID);
            unsigned char status = 0, value = 0;
            dbus_uint16_t length = 1;
            const unsigned char* data = &value;
            DBusMessage* s = dbus_message_new_signal(kAlpsPath, kDm, "BtApplReadConfigurationCfm");
            dbus_message_append_args(s, DBUS_TYPE_BYTE, &status, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &id,
                                     DBUS_TYPE_UINT16, &length, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &data, 1,
                                     DBUS_TYPE_INVALID);
            dbus_connection_send(conn, s, nullptr);
            dbus_message_unref(s);
        }
        dbus_connection_flush(conn);
    }
    // One signal on the service's path, its arguments given as libdbus's varargs.
    DBusMessage* signal(const char* iface, const char* member) {
        return dbus_message_new_signal(kAlpsPath, iface, member);
    }
    void send(DBusMessage* s) {
        dbus_connection_send(conn, s, nullptr);
        dbus_connection_flush(conn);
        dbus_message_unref(s);
        usleep(30000);
    }
};

// The script of signals the fake sends while the link watches: every SPP and DM signal the link reads, with valid
// and invalid signatures, other instances and short addresses, then the service giving up its name.
void playSignals(FakeAlps& fake) {
    fake.stopServing();  // the requests are answered: this thread has the connection to itself
    unsigned char y0 = 0, y1 = 1, y3 = 3, y5 = 5, y0x80 = 0x80, y2 = 2;
    dbus_uint16_t q4 = 4, q6203 = 0x6203, q1 = 1;
    dbus_uint64_t f1 = 1, f2 = 2;
    const unsigned char address[] = {1, 2, 3, 4, 5, 6};
    const unsigned char* a = address;
    const char* text = "odd";
    DBusMessage* s;
    s = fake.signal(kSpp, "BtApplSppSetUuidCfm");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_INVALID);
    fake.send(s);
    for (int instance = 1; instance >= 0; --instance) {
        unsigned char inst = static_cast<unsigned char>(instance);
        s = fake.signal(kSpp, "BtApplSppDataInd");
        dbus_message_append_args(s, DBUS_TYPE_BYTE, &inst, DBUS_TYPE_UINT16, &q4, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &a,
                                 4, DBUS_TYPE_INVALID);
        fake.send(s);
    }
    s = fake.signal(kSpp, "BtApplSppDataCfm");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_BYTE, &y3, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kSpp, "BtApplSppSendInd");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y5, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kSpp, "BtApplSppDataSendFcInd");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_BYTE, &y1, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kSpp, "BtApplOddInd");
    dbus_message_append_args(s, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
    fake.send(s);
    fake.send(fake.signal(kSpp, "BtApplEmptyInd"));
    // Connections: ours, another device's with the status's top bit set, a disconnection, a short address, and a
    // signature that is not the one the link reads.
    s = fake.signal(kDm, "BtApplDeviceConnectInd");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &a, 6, DBUS_TYPE_UINT64, &f1,
                             DBUS_TYPE_BYTE, &y0, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kDm, "BtApplDeviceConnectInd");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0x80, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &a, 6, DBUS_TYPE_UINT64, &f2,
                             DBUS_TYPE_BYTE, &y3, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kDm, "BtApplDeviceDisconnectInd");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &a, 6, DBUS_TYPE_BYTE, &y2,
                             DBUS_TYPE_UINT64, &f1, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kDm, "BtApplDeviceConnectInd");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &a, 5, DBUS_TYPE_UINT64, &f1,
                             DBUS_TYPE_BYTE, &y0, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kDm, "BtApplDeviceConnectInd");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &a, 6, DBUS_TYPE_INVALID);
    fake.send(s);
    // Configuration answers nobody waits for, and one with a signature the link does not read.
    s = fake.signal(kDm, "BtApplSetConfigurationCfm");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_UINT16, &q6203, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kDm, "BtApplReadConfigurationCfm");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_BYTE, &y5, DBUS_TYPE_UINT16, &q6203, DBUS_TYPE_UINT16,
                             &q1, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &a, 1, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kDm, "BtApplReadConfigurationCfm");
    dbus_message_append_args(s, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_INVALID);
    fake.send(s);
    s = fake.signal(kDm, "BtApplSomethingInd");
    dbus_message_append_args(s, DBUS_TYPE_UINT64, &f2, DBUS_TYPE_INVALID);
    fake.send(s);
    // The service goes: the link must say so and stop reading.
    DBusError e;
    dbus_error_init(&e);
    dbus_bus_release_name(fake.conn, "org.alps.evolution", &e);
    dbus_error_free(&e);
    usleep(300000);
}

// A connection subscribed as the link is, recording each signal as the bus delivers it.
struct Subscriber {
    DBusConnection* conn;
    Bytes signals;
    Subscriber() {
        DBusError e;
        dbus_error_init(&e);
        conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
        need(conn != nullptr, "the subscriber's bus");
        const char* rules[] = {
            "type='signal',sender='org.alps.evolution',path='/org/alps/evolution/hli',interface='org.alps.evo.SPP'",
            "type='signal',sender='org.alps.evolution',path='/org/alps/evolution/hli',interface='org.alps.evo.DM'",
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
            "arg0='org.alps.evolution'"};
        for (size_t i = 0; i < 3; ++i) {
            dbus_bus_add_match(conn, rules[i], &e);
            need(!dbus_error_is_set(&e), "a subscriber rule");
        }
    }
    void drain() {
        dbus_connection_read_write(conn, 100);
        DBusMessage* m;
        while ((m = dbus_connection_pop_message(conn)) != nullptr) {
            if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_SIGNAL && !dbus_message_get_destination(m)) {
                char* bytes = nullptr;
                int n = 0;
                need(dbus_message_marshal(m, &bytes, &n), "marshal a signal");
                signals.insert(signals.end(), bytes, bytes + n);
                dbus_free(bytes);
            }
            dbus_message_unref(m);
        }
    }
    ~Subscriber() {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
    }
};

// The link's log lines, without the time each starts with.
std::string withoutTimes(const std::string& log) {
    std::string out;
    size_t at = 0;
    while (at < log.size()) {
        size_t end = log.find('\n', at);
        end = end == std::string::npos ? log.size() : end + 1;
        std::string line = log.substr(at, end - at);
        if (line.size() > 13 && line[0] == '[' && line[10] == ']') {
            line = line.substr(12);
        }
        out += line;
        at = end;
    }
    return out;
}

struct AlpsRecording {
    std::string owner, requests, signals, monitor;
};

AlpsRecording recordAlps() {
    AlpsRecording out;
    FakeAlps fake;
    Subscriber subscriber;
    std::string introspected;
    bool read0 = false, read1 = false;
    std::vector<uint8_t> cfg0, cfg1;
    const std::string log = captured([&] {
        evo::AlpsSppLink bt;
        bt.setMonitorOnly(true);
        need(bt.start(0), "the link starts");
        introspected = bt.introspect(kAlpsPath);
        read0 = bt.readConfiguration(0, 0x6203, cfg0, 2000);
        read1 = bt.readConfiguration(1, 0x6203, cfg1, 2000);
        playSignals(fake);
        bt.stop();
    });
    subscriber.drain();
    need(introspected == kXml, "the fake's introspection read back");
    need(read0 && read1 && cfg0.size() == 1 && cfg1.size() == 1, "both configuration reads");
    out.owner = fake.owner + "\n";
    out.requests.assign(fake.requests.begin(), fake.requests.end());
    out.signals.assign(subscriber.signals.begin(), subscriber.signals.end());
    out.monitor = withoutTimes(log);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FIXTURES_DIR [--record]\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const bool recording = argc >= 3 && !strcmp(argv[2], "--record");
    need(getenv("DBUS_SYSTEM_BUS_ADDRESS") != nullptr, "DBUS_SYSTEM_BUS_ADDRESS (the session bus of dbus-run-session)");
    need(dbus_threads_init_default(), "libdbus threads");
    const AlpsRecording alps = recordAlps();
    const std::string usageText = captured([] { usage(); });
    const std::string files[][2] = {
        {"evo_check/dump.golden", dumpAll(readFile(dir + "/dbus/messages.bin"))},
        {"evo_check/alps-config.golden", alpsConfigAll(dir)},
        {"evo_check/usage.golden", usageText},
        {"evo_check/alps/requests.bin", alps.requests},
        {"evo_check/alps/signals.bin", alps.signals},
        {"evo_check/alps/monitor.golden", alps.monitor},
    };
    int failures = 0;
    // The owner is the bus's choice: recorded once, never compared.
    if (recording) {
        std::ofstream(std::string(dir + "/evo_check/alps/owner.txt").c_str()) << alps.owner;
    }
    const std::string recordedOwner = readFile(dir + "/evo_check/alps/owner.txt");
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        const std::string path = dir + "/" + files[i][0];
        if (recording) {
            std::ofstream out(path.c_str(), std::ios::binary);
            out << files[i][1];
            need(out.good(), "write a fixture");
            printf("recorded %s (%zu bytes)\n", path.c_str(), files[i][1].size());
            continue;
        }
        std::string expected = readFile(path);
        std::string got = files[i][1];
        // The requests and signals name the fake by its unique name, which can differ from run to run.
        if (alps.owner != recordedOwner && files[i][0].find("/alps/") != std::string::npos) {
            const std::string from = alps.owner.substr(0, alps.owner.size() - 1);
            const std::string to = recordedOwner.substr(0, recordedOwner.size() - 1);
            if (from.size() == to.size()) {
                for (size_t at = got.find(from); at != std::string::npos; at = got.find(from, at + to.size())) {
                    got.replace(at, from.size(), to);
                }
            }
        }
        if (expected != got) {
            size_t at = 0;
            while (at < expected.size() && at < got.size() && expected[at] == got[at]) {
                ++at;
            }
            printf("FAIL check golden: %s differs at byte %zu (recorded %zu bytes, the C++ gives %zu)\n", path.c_str(),
                   at, expected.size(), got.size());
            ++failures;
        }
    }
    if (failures) {
        return 1;
    }
    if (!recording) {
        printf(
            "PASS check golden: the dump of the D-Bus corpus, the Alps configuration cases, the usage, and the Alps "
            "link's requests and log as recorded\n");
    }
    return 0;
}
