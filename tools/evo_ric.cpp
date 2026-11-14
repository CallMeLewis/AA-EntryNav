// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// evo-ric: passive recorder of the instrument cluster's guidance calls on the head unit's session bus. On an F-series
// car, BMW's navigation hands its turn-by-turn to the cluster as RICServiceGuidance calls (ric_create, ric_setResource,
// ric_set*Data, ric_initialize...) to a service of its own that draws them over MOST. This records those calls and
// their answers during a factory route, so Android Auto's guidance can be sent the same way.
//
// Built to be safe on a car that restarts easily:
//   - Nothing is sent to any BMW program. The only calls go to the bus itself: ListNames, GetConnectionUnixProcessID
//     and AddMatch for the RIC interface and the answers from its services. Every message received is marked handled,
//     so libdbus never answers an overheard call on our behalf.
//   - Lowest CPU priority, one thread, bounded memory: one line per message, cut at 16 KB.
//   - Written where a restart doesn't reach (default /var/local/data/AA/ric), flushed and synced every 2 s, with a
//     heartbeat line every 30 s that carries the uptime, so a restart shows as a gap. Stops by itself after the given
//     minutes (default 60, at most 240) or at the size limit (default 8 MB, at most 32 MB).
// A developer tool, not part of a release (make dev-tools).
//   evo-ric --names [--address A]                           well-known names on the bus, with their process
//   evo-ric --watch [minutes] [--out DIR] [--max-mb N] [--address A]
//   evo-ric --selftest                                      against the session bus of dbus-run-session (PC)
//   evo-ric --help
#include <dbus/dbus.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

const char* const kCarBus = "unix:path=/tmp/session_bus_socket";
const char* const kIface = "BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.RICServiceGuidance";
// The services that may answer RIC calls: the F-series drawing service (libVMostAdapter's RbRicService) and the two
// instances /etc/CommonApiDBus/dbusAddresses.conf names.
const char* const kServers[] = {"BMW.RueKoIC", "BMW.Kombi", "BMW.NBTEvo"};
const char* const kDefaultOut = "/var/local/data/AA/ric";
const size_t kMaxLine = 16 * 1024;
const unsigned kMaxArray = 64;    // elements printed per array
const unsigned kMaxBytes = 512;   // bytes printed per byte array
const unsigned kMaxString = 400;  // characters printed per string
const unsigned kMaxDynamic = 16;  // answer rules added for callees seen by unique name
const unsigned kSyncEveryMs = 2000;
const unsigned kHeartbeatMs = 30000;

volatile sig_atomic_t g_stop = 0;
void onSignal(int) {
    g_stop = 1;
}

uint64_t monoMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<uint64_t>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

std::string readSmall(const char* path) {
    std::string s;
    FILE* f = fopen(path, "r");
    if (f) {
        char buf[256];
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        s.assign(buf, n);
        fclose(f);
    }
    while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r')) {
        s.erase(s.size() - 1);
    }
    return s;
}

std::string uptime() {
    const std::string u = readSmall("/proc/uptime");
    return u.substr(0, u.find(' '));
}

// ---- formatting --------------------------------------------------------------------------------------------------

void appendf(std::string& out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void appendf(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out += buf;
}

void appendString(std::string& out, const char* s) {
    out += '"';
    unsigned n = 0;
    for (; s && *s && n < kMaxString; ++s, ++n) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            appendf(out, "\\x%02x", c);
        } else {
            out += static_cast<char>(c);
        }
    }
    if (s && *s) {
        out += "...";
    }
    out += '"';
}

// One value at the iterator, its type first for the scalars ("u:7"), recursively for the containers.
void appendValue(std::string& out, DBusMessageIter* it, int depth) {
    if (out.size() > kMaxLine || depth > 32) {
        out += "...";
        return;
    }
    const int type = dbus_message_iter_get_arg_type(it);
    switch (type) {
        case DBUS_TYPE_BYTE: {
            unsigned char v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "y:%u", v);
            break;
        }
        case DBUS_TYPE_BOOLEAN: {
            dbus_bool_t v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "b:%d", v ? 1 : 0);
            break;
        }
        case DBUS_TYPE_INT16: {
            dbus_int16_t v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "n:%d", v);
            break;
        }
        case DBUS_TYPE_UINT16: {
            dbus_uint16_t v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "q:%u", v);
            break;
        }
        case DBUS_TYPE_INT32: {
            dbus_int32_t v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "i:%d", v);
            break;
        }
        case DBUS_TYPE_UINT32: {
            dbus_uint32_t v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "u:%u", v);
            break;
        }
        case DBUS_TYPE_INT64: {
            dbus_int64_t v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "x:%lld", static_cast<long long>(v));
            break;
        }
        case DBUS_TYPE_UINT64: {
            dbus_uint64_t v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "t:%llu", static_cast<unsigned long long>(v));
            break;
        }
        case DBUS_TYPE_DOUBLE: {
            double v = 0;
            dbus_message_iter_get_basic(it, &v);
            appendf(out, "d:%.6g", v);
            break;
        }
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE: {
            const char* v = nullptr;
            dbus_message_iter_get_basic(it, &v);
            out += type == DBUS_TYPE_STRING ? "s:" : type == DBUS_TYPE_OBJECT_PATH ? "o:" : "g:";
            appendString(out, v);
            break;
        }
        case DBUS_TYPE_UNIX_FD:
            out += "h:fd";
            break;
        case DBUS_TYPE_ARRAY: {
            DBusMessageIter sub;
            dbus_message_iter_recurse(it, &sub);
            if (dbus_message_iter_get_element_type(it) == DBUS_TYPE_BYTE) {
                const unsigned char* bytes = nullptr;
                int n = 0;
                dbus_message_iter_get_fixed_array(&sub, &bytes, &n);
                appendf(out, "ay[%d]:", n);
                for (int i = 0; i < n && i < static_cast<int>(kMaxBytes); ++i) {
                    appendf(out, "%02x", bytes[i]);
                }
                if (n > static_cast<int>(kMaxBytes)) {
                    out += "...";
                }
                break;
            }
            out += '[';
            unsigned n = 0;
            while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
                if (n == kMaxArray) {
                    out += ",...";
                    break;
                }
                if (n++) {
                    out += ',';
                }
                appendValue(out, &sub, depth + 1);
                dbus_message_iter_next(&sub);
            }
            out += ']';
            break;
        }
        case DBUS_TYPE_STRUCT:
        case DBUS_TYPE_DICT_ENTRY: {
            DBusMessageIter sub;
            dbus_message_iter_recurse(it, &sub);
            out += type == DBUS_TYPE_STRUCT ? '(' : '{';
            bool first = true;
            while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
                if (!first) {
                    out += ',';
                }
                first = false;
                appendValue(out, &sub, depth + 1);
                dbus_message_iter_next(&sub);
            }
            out += type == DBUS_TYPE_STRUCT ? ')' : '}';
            break;
        }
        case DBUS_TYPE_VARIANT: {
            DBusMessageIter sub;
            dbus_message_iter_recurse(it, &sub);
            char* sig = dbus_message_iter_get_signature(&sub);
            out += "v<";
            out += sig ? sig : "?";
            out += ">";
            dbus_free(sig);
            appendValue(out, &sub, depth + 1);
            break;
        }
        default:
            appendf(out, "?%c", type);
            break;
    }
}

const char* typeName(int type) {
    switch (type) {
        case DBUS_MESSAGE_TYPE_METHOD_CALL:
            return "call";
        case DBUS_MESSAGE_TYPE_METHOD_RETURN:
            return "return";
        case DBUS_MESSAGE_TYPE_ERROR:
            return "error";
        case DBUS_MESSAGE_TYPE_SIGNAL:
            return "signal";
        default:
            return "?";
    }
}

// One line: when, type, serials, who to whom, what, and every argument.
std::string describe(DBusMessage* m, uint64_t t0) {
    std::string line;
    const char* member = dbus_message_get_member(m);
    const char* iface = dbus_message_get_interface(m);
    const char* error = dbus_message_get_error_name(m);
    appendf(line, "%llu %s s=%u rs=%u ", static_cast<unsigned long long>(monoMs() - t0),
            typeName(dbus_message_get_type(m)), dbus_message_get_serial(m), dbus_message_get_reply_serial(m));
    line += dbus_message_get_sender(m) ? dbus_message_get_sender(m) : "-";
    line += " -> ";
    line += dbus_message_get_destination(m) ? dbus_message_get_destination(m) : "-";
    line += ' ';
    line += dbus_message_get_path(m) ? dbus_message_get_path(m) : "-";
    line += ' ';
    line += iface ? iface : "-";
    line += '.';
    line += member ? member : (error ? error : "-");
    const char* sig = dbus_message_get_signature(m);
    line += " sig=";
    line += sig ? sig : "";
    line += " args=";
    DBusMessageIter it;
    if (dbus_message_iter_init(m, &it)) {
        bool first = true;
        do {
            if (!first) {
                line += ' ';
            }
            first = false;
            appendValue(line, &it, 0);
        } while (line.size() <= kMaxLine && dbus_message_iter_next(&it));
    }
    if (line.size() > kMaxLine) {
        line.resize(kMaxLine);
        line += "...";
    }
    return line;
}

// ---- the bus -----------------------------------------------------------------------------------------------------

DBusConnection* openBus(const std::string& address) {
    DBusError e;
    dbus_error_init(&e);
    DBusConnection* c = dbus_connection_open_private(address.c_str(), &e);
    if (c && !dbus_bus_register(c, &e)) {
        dbus_connection_close(c);
        dbus_connection_unref(c);
        c = nullptr;
    }
    if (!c) {
        fprintf(stderr, "evo-ric: cannot open the bus at %s: %s\n", address.c_str(),
                dbus_error_is_set(&e) ? e.message : "?");
    }
    dbus_error_free(&e);
    if (c) {
        dbus_connection_set_exit_on_disconnect(c, FALSE);
    }
    return c;
}

void closeBus(DBusConnection* c) {
    if (c) {
        dbus_connection_close(c);
        dbus_connection_unref(c);
    }
}

// A call to the bus daemon itself; null on failure.
DBusMessage* askBus(DBusConnection* c, const char* member, const char* arg) {
    DBusMessage* m =
        dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", member);
    if (!m) {
        return nullptr;
    }
    if (arg) {
        dbus_message_append_args(m, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 2000, &e);
    dbus_message_unref(m);
    dbus_error_free(&e);
    return r;
}

// The bus's well-known names with the process that owns each, one per line.
void listNames(DBusConnection* c, FILE* out) {
    DBusMessage* r = askBus(c, "ListNames", nullptr);
    if (!r) {
        fprintf(out, "# ListNames: no answer\n");
        return;
    }
    std::vector<std::string> names;
    DBusMessageIter it, sub;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &sub);
        while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
            const char* s = nullptr;
            dbus_message_iter_get_basic(&sub, &s);
            if (s && s[0] != ':') {
                names.push_back(s);
            }
            dbus_message_iter_next(&sub);
        }
    }
    dbus_message_unref(r);
    fprintf(out, "# %u well-known names\n", static_cast<unsigned>(names.size()));
    for (size_t i = 0; i < names.size(); ++i) {
        dbus_uint32_t pid = 0;
        DBusMessage* p = askBus(c, "GetConnectionUnixProcessID", names[i].c_str());
        if (p) {
            dbus_message_get_args(p, nullptr, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
            dbus_message_unref(p);
        }
        char path[64];
        snprintf(path, sizeof(path), "/proc/%u/comm", pid);
        const std::string comm = pid ? readSmall(path) : std::string("?");
        fprintf(out, "# name %s pid %u %s\n", names[i].c_str(), pid, comm.c_str());
    }
}

// ---- watch -------------------------------------------------------------------------------------------------------

struct Watch {
    FILE* file;
    uint64_t t0;
    size_t bytes, maxBytes;
    unsigned calls, answers, signals, dropped;
    bool full;
    std::set<std::string> answering;  // callees whose answers are matched already
    DBusConnection* conn;
};

void matchAnswersFrom(Watch& w, const std::string& sender) {
    if (w.answering.count(sender) || w.answering.size() >= kMaxDynamic + 3) {
        return;
    }
    w.answering.insert(sender);
    const char* const kinds[] = {"method_return", "error"};
    for (int k = 0; k < 2; ++k) {
        const std::string rule = std::string("eavesdrop=true,type='") + kinds[k] + "',sender='" + sender + "'";
        dbus_bus_add_match(w.conn, rule.c_str(), nullptr);  // no wait for the answer: this runs inside dispatch
    }
}

void writeLine(Watch& w, const std::string& line) {
    if (w.full) {
        ++w.dropped;
        return;
    }
    if (w.bytes + line.size() + 1 > w.maxBytes) {
        fprintf(w.file, "# size limit of %u bytes reached: nothing more is recorded\n",
                static_cast<unsigned>(w.maxBytes));
        w.full = true;
        ++w.dropped;
        return;
    }
    fputs(line.c_str(), w.file);
    fputc('\n', w.file);
    w.bytes += line.size() + 1;
}

// Every message is marked handled, whatever it is: an overheard method call left unhandled could be answered by
// libdbus with an error, as if from us, to the program that made it.
DBusHandlerResult onMessage(DBusConnection*, DBusMessage* m, void* user) {
    Watch& w = *static_cast<Watch*>(user);
    const int type = dbus_message_get_type(m);
    if (type == DBUS_MESSAGE_TYPE_SIGNAL && dbus_message_has_interface(m, DBUS_INTERFACE_LOCAL)) {
        if (dbus_message_has_member(m, "Disconnected")) {
            fprintf(w.file, "# the bus closed the connection\n");
            g_stop = 1;
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    const char* from = dbus_message_get_sender(m);
    if (from && !strcmp(from, DBUS_SERVICE_DBUS)) {
        return DBUS_HANDLER_RESULT_HANDLED;  // the bus answering our own AddMatch, and its NameAcquired
    }
    const char* iface = dbus_message_get_interface(m);
    const bool ric = iface && !strcmp(iface, kIface);
    if (type == DBUS_MESSAGE_TYPE_METHOD_CALL && ric) {
        ++w.calls;
        if (dbus_message_get_destination(m)) {
            matchAnswersFrom(w, dbus_message_get_destination(m));  // a callee by unique name: its answers too
        }
    } else if (type == DBUS_MESSAGE_TYPE_SIGNAL && ric) {
        ++w.signals;
    } else if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN || type == DBUS_MESSAGE_TYPE_ERROR) {
        ++w.answers;
    } else if (!ric) {
        return DBUS_HANDLER_RESULT_HANDLED;  // the bus's own NameAcquired and the like
    }
    writeLine(w, describe(m, w.t0));
    return DBUS_HANDLER_RESULT_HANDLED;
}

void flushToDisk(Watch& w) {
    fflush(w.file);
    fsync(fileno(w.file));
}

bool makeDirs(const std::string& dir) {
    std::string partial;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || (dir[i] == '/' && i > 0)) {
            partial = dir.substr(0, i);
            if (mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

// Records until `minutes` pass, a signal, or `stopFlag` (the selftest's).
int watch(const std::string& address, unsigned minutes, const std::string& dir, size_t maxBytes,
          volatile sig_atomic_t* stopFlag, std::string* fileOut) {
    if (!makeDirs(dir)) {
        fprintf(stderr, "evo-ric: cannot create %s: %s\n", dir.c_str(), strerror(errno));
        return 1;
    }
    std::string boot = readSmall("/proc/sys/kernel/random/boot_id");
    boot = boot.substr(0, 8);
    char name[512];
    snprintf(name, sizeof(name), "%s/ric-%s-%ld.log", dir.c_str(), boot.empty() ? "noboot" : boot.c_str(),
             static_cast<long>(time(nullptr)));
    if (fileOut) {
        *fileOut = name;
    }
    Watch w;
    w.file = fopen(name, "w");
    if (!w.file) {
        fprintf(stderr, "evo-ric: cannot write %s: %s\n", name, strerror(errno));
        return 1;
    }
    w.t0 = monoMs();
    w.bytes = 0;
    w.maxBytes = maxBytes;
    w.calls = w.answers = w.signals = w.dropped = 0;
    w.full = false;
    w.conn = openBus(address);
    if (!w.conn) {
        fprintf(w.file, "# cannot open the bus at %s\n", address.c_str());
        fclose(w.file);
        return 1;
    }
    char started[64];
    const time_t now = time(nullptr);
    strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(w.file, "# evo-ric watch: %s, boot %s, uptime %s s, bus %s, %u min at most, %u bytes at most\n", started,
            boot.c_str(), uptime().c_str(), address.c_str(), minutes, static_cast<unsigned>(maxBytes));
    fprintf(w.file,
            "# line: ms since start, type, serial, reply serial, sender -> destination, path, member, "
            "signature, arguments\n");
    listNames(w.conn, w.file);
    // The interface's calls and signals, and the answers from the services that may serve it.
    std::vector<std::string> rules;
    rules.push_back(std::string("eavesdrop=true,type='method_call',interface='") + kIface + "'");
    rules.push_back(std::string("eavesdrop=true,type='signal',interface='") + kIface + "'");
    for (size_t i = 0; i < sizeof(kServers) / sizeof(kServers[0]); ++i) {
        w.answering.insert(kServers[i]);
        rules.push_back(std::string("eavesdrop=true,type='method_return',sender='") + kServers[i] + "'");
        rules.push_back(std::string("eavesdrop=true,type='error',sender='") + kServers[i] + "'");
    }
    for (size_t i = 0; i < rules.size(); ++i) {
        DBusError e;
        dbus_error_init(&e);
        dbus_bus_add_match(w.conn, rules[i].c_str(), &e);
        fprintf(w.file, "# match %s: %s\n", rules[i].c_str(), dbus_error_is_set(&e) ? e.message : "accepted");
        dbus_error_free(&e);
    }
    dbus_connection_add_filter(w.conn, onMessage, &w, nullptr);
    flushToDisk(w);
    const uint64_t until = w.t0 + static_cast<uint64_t>(minutes) * 60000;
    uint64_t syncedAt = monoMs(), beatAt = monoMs();
    while (!g_stop && !(stopFlag && *stopFlag) && monoMs() < until) {
        if (!dbus_connection_read_write_dispatch(w.conn, 500)) {
            fprintf(w.file, "# the bus connection was lost\n");
            break;
        }
        const uint64_t t = monoMs();
        if (t - beatAt >= kHeartbeatMs) {
            fprintf(w.file, "# alive %llu ms, uptime %s s, %u calls, %u answers, %u signals, %u bytes, %u dropped\n",
                    static_cast<unsigned long long>(t - w.t0), uptime().c_str(), w.calls, w.answers, w.signals,
                    static_cast<unsigned>(w.bytes), w.dropped);
            beatAt = t;
        }
        if (t - syncedAt >= kSyncEveryMs) {
            flushToDisk(w);
            syncedAt = t;
        }
    }
    fprintf(w.file, "# end after %llu ms: %u calls, %u answers, %u signals, %u bytes, %u dropped\n",
            static_cast<unsigned long long>(monoMs() - w.t0), w.calls, w.answers, w.signals,
            static_cast<unsigned>(w.bytes), w.dropped);
    flushToDisk(w);
    fclose(w.file);
    dbus_connection_remove_filter(w.conn, onMessage, &w);
    closeBus(w.conn);
    printf("evo-ric: %u calls, %u answers, %u signals recorded in %s\n", w.calls, w.answers, w.signals, name);
    return 0;
}

// ---- selftest (PC, under dbus-run-session) -------------------------------------------------------------------------

struct Selftest {
    volatile sig_atomic_t stop;
    std::string address, dir, file;
    int result;
};

void* watchThread(void* arg) {
    Selftest* s = static_cast<Selftest*>(arg);
    s->result = watch(s->address, 1, s->dir, 1 << 20, &s->stop, &s->file);
    return nullptr;
}

unsigned g_served = 0;
DBusHandlerResult serve(DBusConnection* c, DBusMessage* m, void*) {
    if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL && dbus_message_has_interface(m, kIface)) {
        ++g_served;
        DBusMessage* r = dbus_message_new_method_return(m);
        dbus_int32_t status = 0;
        dbus_message_append_args(r, DBUS_TYPE_INT32, &status, DBUS_TYPE_INVALID);
        dbus_connection_send(c, r, nullptr);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

std::string readAll(const std::string& path) {
    std::string s;
    FILE* f = fopen(path.c_str(), "r");
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        fclose(f);
    }
    return s;
}

int selftest() {
    const char* address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!address) {
        fprintf(stderr, "FAIL evo-ric selftest: no session bus (run it under dbus-run-session)\n");
        return 1;
    }
    char dir[] = "/tmp/evo-ric-test-XXXXXX";
    if (!mkdtemp(dir)) {
        return 1;
    }
    // The service, as BMW's navigation runs it, and a caller.
    DBusConnection* server = openBus(address);
    DBusConnection* caller = openBus(address);
    if (!server || !caller) {
        return 1;
    }
    DBusError e;
    dbus_error_init(&e);
    dbus_bus_request_name(server, "BMW.RueKoIC", DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    dbus_connection_add_filter(server, serve, nullptr, nullptr);
    Selftest s;
    s.stop = 0;
    s.address = address;
    s.dir = dir;
    s.result = -1;
    pthread_t t;
    pthread_create(&t, nullptr, watchThread, &s);
    usleep(500000);  // its rules in place
    // A RIC call with a struct and a variant, answered by the service; then a call on another interface.
    DBusMessage* call = dbus_message_new_method_call("BMW.RueKoIC", "/BMW/RueKoIC", kIface, "ric_setResource");
    DBusMessageIter it, st, var;
    dbus_message_iter_init_append(call, &it);
    dbus_uint32_t handle = 7, id = 0x03000000;
    dbus_int32_t kind = 3;
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &handle);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &id);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &kind);
    dbus_message_iter_open_container(&it, DBUS_TYPE_STRUCT, nullptr, &st);
    unsigned char index = 9;
    dbus_message_iter_append_basic(&st, DBUS_TYPE_BYTE, &index);
    dbus_message_iter_open_container(&st, DBUS_TYPE_VARIANT, "s", &var);
    const char* text = "Main St";
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &text);
    dbus_message_iter_close_container(&st, &var);
    dbus_message_iter_close_container(&it, &st);
    DBusPendingCall* pending = nullptr;
    dbus_connection_send_with_reply(caller, call, &pending, 3000);
    dbus_message_unref(call);
    DBusMessage* other =
        dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetId");
    dbus_connection_send(caller, other, nullptr);
    dbus_message_unref(other);
    dbus_connection_flush(caller);
    for (int i = 0; i < 100 && !g_served; ++i) {
        dbus_connection_read_write_dispatch(server, 20);
    }
    for (int i = 0; i < 50; ++i) {
        dbus_connection_read_write_dispatch(caller, 20);
    }
    DBusMessage* reply = nullptr;
    if (pending) {
        dbus_pending_call_block(pending);
        reply = dbus_pending_call_steal_reply(pending);
        dbus_pending_call_unref(pending);
    }
    const bool answered = reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN;
    if (reply) {
        dbus_message_unref(reply);
    }
    usleep(500000);
    s.stop = 1;
    pthread_join(t, nullptr);
    const std::string log = readAll(s.file);
    int failures = 0;
    struct Expect {
        const char* text;
        bool present;
    } const expects[] = {
        {"call s=", true},
        {"BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.RICServiceGuidance.ric_setResource sig=uui(yv) args=u:7 "
         "u:50331648 i:3 (y:9,v<s>s:\"Main St\")",
         true},
        {"return s=", true},
        {"sig=i args=i:0", true},
        {"GetId", false},
        {"# match eavesdrop=true,type='method_call'", true},
        {"# end after", true},
    };
    for (size_t i = 0; i < sizeof(expects) / sizeof(expects[0]); ++i) {
        if ((log.find(expects[i].text) != std::string::npos) != expects[i].present) {
            fprintf(stderr, "FAIL evo-ric selftest: %s \"%s\"\n", expects[i].present ? "missing" : "unexpected",
                    expects[i].text);
            ++failures;
        }
    }
    if (s.result != 0 || !answered || g_served != 1) {
        fprintf(stderr, "FAIL evo-ric selftest: watch %d, answered %d, served %u\n", s.result, answered ? 1 : 0,
                g_served);
        ++failures;
    }
    if (failures) {
        fprintf(stderr, "%s", log.c_str());
    }
    unlink(s.file.c_str());
    rmdir(dir);
    closeBus(server);
    closeBus(caller);
    dbus_error_free(&e);
    if (failures) {
        return 1;
    }
    puts(
        "PASS evo-ric: the RIC call and its answer recorded with every argument, other traffic left out, the "
        "caller answered only by the service");
    return 0;
}

unsigned clampArg(const char* text, unsigned dflt, unsigned lo, unsigned hi) {
    if (!text) {
        return dflt;
    }
    const long v = strtol(text, nullptr, 10);
    return v < static_cast<long>(lo) ? lo : v > static_cast<long>(hi) ? hi : static_cast<unsigned>(v);
}

const char* const kUsage =
    "usage: evo-ric --names [--address A]\n"
    "       evo-ric --watch [minutes] [--out DIR] [--max-mb N] [--address A]\n"
    "       evo-ric --selftest\n";

int usage() {
    fputs(kUsage, stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    dbus_threads_init_default();
    if (argc < 2) {
        return usage();
    }
    const std::string mode = argv[1];
    if (mode == "--selftest") {
        return selftest();
    }
    if (mode == "--help") {
        fputs(kUsage, stdout);  // also how QEMU checks that it loads with the car's libdbus
        return 0;
    }
    std::string address = getenv("DBUS_SESSION_BUS_ADDRESS") ? getenv("DBUS_SESSION_BUS_ADDRESS") : kCarBus;
    std::string dir = kDefaultOut;
    unsigned minutes = 60, maxMb = 8;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--address" && i + 1 < argc) {
            address = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--max-mb" && i + 1 < argc) {
            maxMb = clampArg(argv[++i], 8, 1, 32);
        } else if (mode == "--watch" && a[0] != '-') {
            minutes = clampArg(a.c_str(), 60, 1, 240);
        } else {
            return usage();
        }
    }
    setpriority(PRIO_PROCESS, 0, 19);  // the car's own programs first
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP, SIG_IGN);  // the SSH session that started it may go
    signal(SIGPIPE, SIG_IGN);
    if (mode == "--names") {
        DBusConnection* c = openBus(address);
        if (!c) {
            return 1;
        }
        printf("# bus %s\n", address.c_str());
        listNames(c, stdout);
        closeBus(c);
        return 0;
    }
    if (mode == "--watch") {
        return watch(address, minutes, dir, static_cast<size_t>(maxMb) << 20, nullptr, nullptr);
    }
    return usage();
}
