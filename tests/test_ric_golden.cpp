// Copyright (c) 2026 CallMeLewis
// The C++ evo-ric's outputs, recorded for its Rust port (rust/evo-ric) and checked against what was recorded:
//   dbus/messages.bin       a fixed set of D-Bus messages as the bus delivers them, made and read back by libdbus
//                           (rust/aa-dbus and evo-check's recorder read it too)
//   evo_ric/describe.golden the line evo-ric records for each message, its time field counting 37 ms per message
//   evo_ric/watch.golden    what evo-ric's watch writes for the whole set, with its counts and the callees whose
//                           answers it then matches; watch-small.golden the same with a 3000 byte limit
//   evo_ric/selftest.golden what evo-ric --selftest prints, the log's path written FILE
//   evo_ric/usage.golden    evo-ric's usage text
// rust/evo-ric/tests/golden.rs reads the same files and must produce the same bytes.
//   test-ric-golden DIR            check that the C++ still gives what DIR (tests/fixtures) holds (make test-host)
//   test-ric-golden DIR --record   write them again (only when the C++'s behaviour is meant to change)
// Both run under dbus-run-session: the watch needs a bus connection, and the self test a bus.
#include <time.h>

#include <cstdint>

// evo-ric's clock, so that the time on each recorded line is chosen here.
static uint64_t g_fakeMs = 1000;
extern "C" int goldenClock(clockid_t, struct timespec* t) {
    t->tv_sec = static_cast<time_t>(g_fakeMs / 1000);
    t->tv_nsec = static_cast<long>(g_fakeMs % 1000) * 1000000L;
    return 0;
}
#define clock_gettime goldenClock
#define main evo_ric_main
#include "../tools/evo_ric.cpp"
#undef main
#undef clock_gettime

#include <fcntl.h>

#include <fstream>
#include <iterator>

namespace {

typedef std::vector<uint8_t> Bytes;

// Reproducible pseudo-random numbers, so that --record writes the same corpus again.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint32_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 33);
    }
    uint32_t below(uint32_t n) { return next() % n; }
    uint64_t wide() { return (static_cast<uint64_t>(next()) << 32) ^ next() ^ (static_cast<uint64_t>(next()) << 11); }
};

void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL ric golden: %s\n", what);
        exit(1);
    }
}

// ---- values ------------------------------------------------------------------------------------------------------

size_t typeEnd(const std::string& s, size_t i) {
    const char c = s[i];
    if (c == 'a') {
        return typeEnd(s, i + 1);
    }
    if (c == '(' || c == '{') {
        const char close = c == '(' ? ')' : '}';
        size_t at = i + 1;
        while (s[at] != close) {
            at = typeEnd(s, at);
        }
        return at + 1;
    }
    return i + 1;
}

std::vector<std::string> splitTypes(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const size_t end = typeEnd(s, i);
        out.push_back(s.substr(i, end - i));
        i = end;
    }
    return out;
}

const char kBasic[] = "ybnqiuxtdsog";

std::string randomType(Rng& r, int depth) {
    const uint32_t k = r.below(depth >= 3 ? 12 : 18);
    if (k < 12) {
        return std::string(1, kBasic[k]);
    }
    switch (k) {
        case 12:
            return "v";
        case 13:
            return "a" + randomType(r, depth + 1);
        case 14: {
            std::string s = "(";
            const uint32_t n = 1 + r.below(4);
            for (uint32_t i = 0; i < n; ++i) {
                s += randomType(r, depth + 1);
            }
            return s + ")";
        }
        case 15:
            return std::string("a{") + kBasic[r.below(12)] + randomType(r, depth + 1) + "}";
        case 16:
            return "ay";
        default:
            return "as";
    }
}

void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xc0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xe0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        s += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        s += static_cast<char>(0xf0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        s += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

// Text with the characters evo-ric escapes or passes as they are: controls, quotes, backslashes, DEL and UTF-8.
std::string randomText(Rng& r) {
    static const uint32_t kPoints[] = {'a',  'Z',  ' ',  '"',  '\\',   '\t',    '\n',
                                       0x01, 0x1f, 0x7f, 0xe9, 0x20ac, 0x1f600, '.'};
    const uint32_t n = r.below(24) == 0 ? 380 + r.below(40) : r.below(30);
    std::string s;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t pick =
            r.below(3) == 0 ? kPoints[r.below(sizeof(kPoints) / sizeof(kPoints[0]))] : 'a' + r.below(26);
        appendUtf8(s, pick);
    }
    return s;
}

std::string randomPath(Rng& r) {
    std::string s;
    const uint32_t parts = r.below(4);
    for (uint32_t i = 0; i < parts; ++i) {
        s += '/';
        const uint32_t n = 1 + r.below(8);
        for (uint32_t k = 0; k < n; ++k) {
            s += "abcXYZ019_"[r.below(10)];
        }
    }
    return s.empty() ? "/" : s;
}

double randomDouble(Rng& r) {
    static const double kSpecial[] = {0.0,
                                      -0.0,
                                      1.0,
                                      0.1,
                                      1e-5,
                                      1e-4,
                                      123456.0,
                                      1234567.0,
                                      999999.5,
                                      1e100,
                                      -2.5e-300,
                                      5e-324,
                                      1.7976931348623157e308,
                                      3.141592653589793,
                                      -42.0,
                                      0.5};
    const uint32_t k = r.below(24);
    if (k < sizeof(kSpecial) / sizeof(kSpecial[0])) {
        return kSpecial[k];
    }
    uint64_t bits = r.wide();
    double d;
    memcpy(&d, &bits, sizeof(d));  // any double, NaNs and infinities among them
    return d;
}

void appendRandom(DBusMessageIter* it, const std::string& type, Rng& r, int depth);

void appendBasic(DBusMessageIter* it, char code, Rng& r) {
    switch (code) {
        case 'y': {
            unsigned char v = static_cast<unsigned char>(r.next());
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_BYTE, &v), "append y");
            break;
        }
        case 'b': {
            dbus_bool_t v = r.below(2);
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &v), "append b");
            break;
        }
        case 'n': {
            dbus_int16_t v = static_cast<dbus_int16_t>(r.next());
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_INT16, &v), "append n");
            break;
        }
        case 'q': {
            dbus_uint16_t v = static_cast<dbus_uint16_t>(r.next());
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_UINT16, &v), "append q");
            break;
        }
        case 'i': {
            dbus_int32_t v = static_cast<dbus_int32_t>(r.next());
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &v), "append i");
            break;
        }
        case 'u': {
            dbus_uint32_t v = r.next();
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_UINT32, &v), "append u");
            break;
        }
        case 'x': {
            dbus_int64_t v = static_cast<dbus_int64_t>(r.wide());
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_INT64, &v), "append x");
            break;
        }
        case 't': {
            dbus_uint64_t v = r.wide();
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_UINT64, &v), "append t");
            break;
        }
        case 'd': {
            double v = randomDouble(r);
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_DOUBLE, &v), "append d");
            break;
        }
        case 's': {
            const std::string s = randomText(r);
            const char* p = s.c_str();
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &p), "append s");
            break;
        }
        case 'o': {
            const std::string s = randomPath(r);
            const char* p = s.c_str();
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &p), "append o");
            break;
        }
        default: {
            const std::string s = randomType(r, 0) + randomType(r, 1);
            const char* p = s.c_str();
            check(dbus_message_iter_append_basic(it, DBUS_TYPE_SIGNATURE, &p), "append g");
            break;
        }
    }
}

void appendRandom(DBusMessageIter* it, const std::string& type, Rng& r, int depth) {
    DBusMessageIter sub;
    switch (type[0]) {
        case 'a': {
            const std::string element = type.substr(1);
            check(dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, element.c_str(), &sub), "open a");
            if (element == "y") {
                Bytes bytes(r.below(6) == 0 ? 505 + r.below(15) : r.below(12));
                for (size_t i = 0; i < bytes.size(); ++i) {
                    bytes[i] = static_cast<uint8_t>(r.next());
                }
                const uint8_t* p = bytes.data();
                check(dbus_message_iter_append_fixed_array(&sub, DBUS_TYPE_BYTE, &p, static_cast<int>(bytes.size())),
                      "append ay");
            } else {
                const uint32_t n = r.below(30) == 0 ? 60 + r.below(10) : r.below(4);
                for (uint32_t i = 0; i < n && depth < 6; ++i) {
                    appendRandom(&sub, element, r, depth + 1);
                }
            }
            check(dbus_message_iter_close_container(it, &sub), "close a");
            break;
        }
        case '(': {
            check(dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, nullptr, &sub), "open (");
            const std::vector<std::string> fields = splitTypes(type.substr(1, type.size() - 2));
            for (size_t i = 0; i < fields.size(); ++i) {
                appendRandom(&sub, fields[i], r, depth + 1);
            }
            check(dbus_message_iter_close_container(it, &sub), "close (");
            break;
        }
        case '{': {
            check(dbus_message_iter_open_container(it, DBUS_TYPE_DICT_ENTRY, nullptr, &sub), "open {");
            const std::vector<std::string> fields = splitTypes(type.substr(1, type.size() - 2));
            appendRandom(&sub, fields[0], r, depth + 1);
            appendRandom(&sub, fields[1], r, depth + 1);
            check(dbus_message_iter_close_container(it, &sub), "close {");
            break;
        }
        case 'v': {
            const std::string inner = depth < 5 ? randomType(r, 2) : "i";
            check(dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, inner.c_str(), &sub), "open v");
            appendRandom(&sub, inner, r, depth + 1);
            check(dbus_message_iter_close_container(it, &sub), "close v");
            break;
        }
        default:
            appendBasic(it, type[0], r);
            break;
    }
}

// ---- the corpus --------------------------------------------------------------------------------------------------

struct Corpus {
    Bytes wire;                      // every message as delivered, back to back
    std::vector<DBusMessage*> read;  // the same, as libdbus reads them back
    uint32_t serial;
    Corpus() : serial(100) {}

    // Delivers m as the bus would: a serial, the sender the bus sets, then marshalled and read back.
    void add(DBusMessage* m, const char* sender) {
        check(m != nullptr, "a message");
        dbus_message_set_serial(m, ++serial);
        if (sender) {
            check(dbus_message_set_sender(m, sender), "set sender");
        }
        char* bytes = nullptr;
        int n = 0;
        check(dbus_message_marshal(m, &bytes, &n), "marshal");
        dbus_message_unref(m);
        addRaw(Bytes(bytes, bytes + n));
        dbus_free(bytes);
    }
    void addRaw(const Bytes& bytes) {
        DBusError e;
        dbus_error_init(&e);
        DBusMessage* back =
            dbus_message_demarshal(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), &e);
        if (!back) {
            fprintf(stderr, "FAIL ric golden: libdbus refused message %zu: %s\n", read.size(), e.message);
            exit(1);
        }
        wire.insert(wire.end(), bytes.begin(), bytes.end());
        read.push_back(back);
    }
};

DBusMessage* ricCall(const char* destination, const char* member) {
    return dbus_message_new_method_call(destination, "/BMW/RueKoIC", kIface, member);
}

// A hand-made big-endian message, as a program on a big-endian machine would send it: libdbus writes its own order.
struct BigEndian {
    Bytes v;
    void align(size_t n) {
        while (v.size() % n) {
            v.push_back(0);
        }
    }
    void u8(unsigned x) { v.push_back(static_cast<uint8_t>(x)); }
    void u16(unsigned x) {
        align(2);
        u8(x >> 8);
        u8(x);
    }
    void u32(uint32_t x) {
        align(4);
        u16(x >> 16);
        u16(x & 0xffff);
    }
    void u64(uint64_t x) {
        align(8);
        u32(static_cast<uint32_t>(x >> 32));
        u32(static_cast<uint32_t>(x));
    }
    void str(const char* s) {
        u32(static_cast<uint32_t>(strlen(s)));
        v.insert(v.end(), s, s + strlen(s) + 1);
    }
    void sig(const char* s) {
        u8(static_cast<unsigned>(strlen(s)));
        v.insert(v.end(), s, s + strlen(s) + 1);
    }
    void field(unsigned code, const char* type, const char* value) {
        align(8);
        u8(code);
        sig(type);
        if (!strcmp(type, "g")) {
            sig(value);
        } else {
            str(value);
        }
    }
};

Bytes bigEndianMessage(unsigned type, uint32_t serial, const char* member, const char* signature, bool fullBody) {
    BigEndian h;
    h.u8('B');
    h.u8(type);
    h.u8(0);
    h.u8(1);
    h.u32(0);  // body length, below
    h.u32(serial);
    h.u32(0);  // field array length, below
    h.field(1, "o", "/BMW/RueKoIC");
    h.field(2, "s", kIface);
    h.field(3, "s", member);
    h.field(8, "g", signature);
    h.field(7, "s", ":1.33");
    const uint32_t fields = static_cast<uint32_t>(h.v.size() - 16);
    h.v[12] = static_cast<uint8_t>(fields >> 24);
    h.v[13] = static_cast<uint8_t>(fields >> 16);
    h.v[14] = static_cast<uint8_t>(fields >> 8);
    h.v[15] = static_cast<uint8_t>(fields);
    h.align(8);
    BigEndian b;
    if (fullBody) {  // ynqiuxtdsoas
        b.u8(200);
        b.u16(static_cast<uint16_t>(-2));
        b.u16(65535);
        b.u32(static_cast<uint32_t>(-100000));
        b.u32(4000000000u);
        b.u64(static_cast<uint64_t>(-1));
        b.u64(18446744073709551615ULL);
        double d = 3.25;
        uint64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        b.u64(bits);
        b.str("Big end");
        b.str("/a/b");
        b.u32(0);
        const size_t lengthAt = b.v.size() - 4;
        const size_t start = b.v.size();
        b.str("x");
        b.str("yz");
        const uint32_t n = static_cast<uint32_t>(b.v.size() - start);
        b.v[lengthAt] = static_cast<uint8_t>(n >> 24);
        b.v[lengthAt + 1] = static_cast<uint8_t>(n >> 16);
        b.v[lengthAt + 2] = static_cast<uint8_t>(n >> 8);
        b.v[lengthAt + 3] = static_cast<uint8_t>(n);
    } else {  // a(ui)
        b.u32(0);
        b.align(8);
        const size_t start = b.v.size();
        b.u32(7);
        b.u32(static_cast<uint32_t>(-7));
        b.align(8);
        b.u32(8);
        b.u32(9);
        const uint32_t n = static_cast<uint32_t>(b.v.size() - start);
        b.v[start - 8] = static_cast<uint8_t>(n >> 24);
        b.v[start - 7] = static_cast<uint8_t>(n >> 16);
        b.v[start - 6] = static_cast<uint8_t>(n >> 8);
        b.v[start - 5] = static_cast<uint8_t>(n);
    }
    const uint32_t body = static_cast<uint32_t>(b.v.size());
    h.v[4] = static_cast<uint8_t>(body >> 24);
    h.v[5] = static_cast<uint8_t>(body >> 16);
    h.v[6] = static_cast<uint8_t>(body >> 8);
    h.v[7] = static_cast<uint8_t>(body);
    h.v.insert(h.v.end(), b.v.begin(), b.v.end());
    return h.v;
}

void appendText(DBusMessageIter* it, const std::string& s) {
    const char* p = s.c_str();
    check(dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &p), "append text");
}

Corpus buildCorpus() {
    Corpus c;
    // The self test's call and its answer.
    {
        DBusMessage* m = ricCall("BMW.RueKoIC", "ric_setResource");
        DBusMessageIter it, st, var;
        dbus_message_iter_init_append(m, &it);
        dbus_uint32_t handle = 7, id = 0x03000000;
        dbus_int32_t kind = 3;
        dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &handle);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &id);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &kind);
        dbus_message_iter_open_container(&it, DBUS_TYPE_STRUCT, nullptr, &st);
        unsigned char index = 9;
        dbus_message_iter_append_basic(&st, DBUS_TYPE_BYTE, &index);
        dbus_message_iter_open_container(&st, DBUS_TYPE_VARIANT, "s", &var);
        appendText(&var, "Main St");
        dbus_message_iter_close_container(&st, &var);
        dbus_message_iter_close_container(&it, &st);
        c.add(m, ":1.20");
    }
    {
        DBusMessage* call = ricCall("BMW.RueKoIC", "ric_setResource");
        dbus_message_set_serial(call, c.serial);
        dbus_message_set_sender(call, ":1.20");
        DBusMessage* r = dbus_message_new_method_return(call);
        dbus_int32_t status = 0;
        dbus_message_append_args(r, DBUS_TYPE_INT32, &status, DBUS_TYPE_INVALID);
        dbus_message_unref(call);
        c.add(r, ":1.5");
    }
    // An error answer, a RIC signal, a call on another interface and the bus's own signal: the last two left out.
    {
        DBusMessage* call = ricCall("BMW.Kombi", "ric_initialize");
        dbus_message_set_serial(call, 41);
        dbus_message_set_sender(call, ":1.21");
        DBusMessage* r = dbus_message_new_error(call, "org.freedesktop.DBus.Error.UnknownMethod", "No \"such\" method");
        dbus_message_unref(call);
        c.add(r, ":1.6");
    }
    {
        DBusMessage* m = dbus_message_new_signal("/BMW/RueKoIC", kIface, "ric_statusChanged");
        dbus_uint32_t v = 4294967295u;
        dbus_message_append_args(m, DBUS_TYPE_UINT32, &v, DBUS_TYPE_INVALID);
        c.add(m, ":1.5");
    }
    c.add(
        dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetId"),
        ":1.20");
    {
        DBusMessage* m = dbus_message_new_signal("/org/freedesktop/DBus", "org.freedesktop.DBus", "NameAcquired");
        dbus_message_set_destination(m, ":1.9");
        const char* name = ":1.9";
        dbus_message_append_args(m, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
        c.add(m, "org.freedesktop.DBus");
    }
    // A call with no interface, one with no destination, and calls to callees by unique name, past the limit of
    // 16 extra answer rules.
    c.add(dbus_message_new_method_call("BMW.RueKoIC", "/BMW/RueKoIC", nullptr, "ric_setData"), ":1.20");
    c.add(ricCall(nullptr, "ric_broadcast"), ":1.20");
    for (int i = 0; i < 20; ++i) {
        char name[16];
        snprintf(name, sizeof(name), ":1.%d", 50 + i % 18);
        c.add(ricCall(name, "ric_setNavigationData"), ":1.20");
    }
    // Answers and errors with no arguments, a return to someone else, every scalar's limits.
    {
        DBusMessage* call = ricCall(":1.50", "ric_create");
        dbus_message_set_serial(call, 4294967295u);
        dbus_message_set_sender(call, ":1.20");
        c.add(dbus_message_new_method_return(call), ":1.50");
        c.add(dbus_message_new_error(call, "BMW.Error.Busy", nullptr), ":1.50");
        dbus_message_unref(call);
    }
    {
        DBusMessage* m = ricCall("BMW.NBTEvo", "ric_limits");
        unsigned char y0 = 0, y1 = 255;
        dbus_bool_t b0 = 0, b1 = 1;
        dbus_int16_t n0 = -32768, n1 = 32767;
        dbus_uint16_t q = 65535;
        dbus_int32_t i0 = -2147483647 - 1, i1 = 2147483647;
        dbus_uint32_t u = 4294967295u;
        dbus_int64_t x0 = INT64_MIN, x1 = INT64_MAX;
        dbus_uint64_t t = UINT64_MAX;
        double d0 = 1234567.0, d1 = -0.0, d2 = 0.0001;
        dbus_message_append_args(m, DBUS_TYPE_BYTE, &y0, DBUS_TYPE_BYTE, &y1, DBUS_TYPE_BOOLEAN, &b0, DBUS_TYPE_BOOLEAN,
                                 &b1, DBUS_TYPE_INT16, &n0, DBUS_TYPE_INT16, &n1, DBUS_TYPE_UINT16, &q, DBUS_TYPE_INT32,
                                 &i0, DBUS_TYPE_INT32, &i1, DBUS_TYPE_UINT32, &u, DBUS_TYPE_INT64, &x0, DBUS_TYPE_INT64,
                                 &x1, DBUS_TYPE_UINT64, &t, DBUS_TYPE_DOUBLE, &d0, DBUS_TYPE_DOUBLE, &d1,
                                 DBUS_TYPE_DOUBLE, &d2, DBUS_TYPE_INVALID);
        c.add(m, ":1.20");
    }
    // Strings at the 400 character limit, with UTF-8 cut at it, and every escape.
    {
        DBusMessage* m = ricCall("BMW.RueKoIC", "ric_setText");
        DBusMessageIter it;
        dbus_message_iter_init_append(m, &it);
        appendText(&it, std::string(399, 'a'));
        appendText(&it, std::string(400, 'b'));
        appendText(&it, std::string(401, 'c'));
        std::string cut(399, 'd');
        appendUtf8(cut, 0x20ac);
        appendText(&it, cut);
        appendText(&it, "quote \" backslash \\ tab \t newline \n bell \x07 del \x7f euro \xe2\x82\xac");
        appendText(&it, "");
        const char* path = "/BMW/Rue_Ko9";
        dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
        const char* sig = "a{sv}(iay)";
        dbus_message_iter_append_basic(&it, DBUS_TYPE_SIGNATURE, &sig);
        c.add(m, ":1.20");
    }
    // Arrays at the limits: 64 and 65 elements, 512 and 513 bytes, empty ones, dictionaries, arrays of arrays.
    {
        DBusMessage* m = ricCall("BMW.RueKoIC", "ric_setArrays");
        DBusMessageIter it, a, inner, entry, var;
        dbus_message_iter_init_append(m, &it);
        for (int count = 64; count <= 65; ++count) {
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "i", &a);
            for (dbus_int32_t k = 0; k < count; ++k) {
                dbus_message_iter_append_basic(&a, DBUS_TYPE_INT32, &k);
            }
            dbus_message_iter_close_container(&it, &a);
        }
        for (int count = 512; count <= 513; ++count) {
            Bytes bytes(count);
            for (int k = 0; k < count; ++k) {
                bytes[k] = static_cast<uint8_t>(k * 7);
            }
            const uint8_t* p = bytes.data();
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "y", &a);
            dbus_message_iter_append_fixed_array(&a, DBUS_TYPE_BYTE, &p, count);
            dbus_message_iter_close_container(&it, &a);
        }
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "y", &a);
        dbus_message_iter_close_container(&it, &a);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(ii)", &a);
        dbus_message_iter_close_container(&it, &a);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &a);
        for (int k = 0; k < 2; ++k) {
            dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            appendText(&entry, k ? "speed" : "street");
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, k ? "d" : "s", &var);
            if (k) {
                double v = 13.5;
                dbus_message_iter_append_basic(&var, DBUS_TYPE_DOUBLE, &v);
            } else {
                appendText(&var, "Main St");
            }
            dbus_message_iter_close_container(&entry, &var);
            dbus_message_iter_close_container(&a, &entry);
        }
        dbus_message_iter_close_container(&it, &a);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "ai", &a);
        for (dbus_int32_t k = 0; k < 2; ++k) {
            dbus_message_iter_open_container(&a, DBUS_TYPE_ARRAY, "i", &inner);
            dbus_message_iter_append_basic(&inner, DBUS_TYPE_INT32, &k);
            dbus_message_iter_close_container(&a, &inner);
        }
        dbus_message_iter_close_container(&it, &a);
        c.add(m, ":1.20");
    }
    // Nesting past evo-ric's depth of 32: arrays in arrays around structs in structs.
    {
        DBusMessage* m = ricCall("BMW.RueKoIC", "ric_setDeep");
        DBusMessageIter levels[40];
        dbus_message_iter_init_append(m, &levels[0]);
        std::string sig = std::string(20, 'a') + std::string(15, '(') + "i" + std::string(15, ')');
        int depth = 0;
        for (int k = 0; k < 20; ++k, ++depth) {
            dbus_message_iter_open_container(&levels[depth], DBUS_TYPE_ARRAY, sig.c_str() + k + 1, &levels[depth + 1]);
        }
        for (int k = 0; k < 15; ++k, ++depth) {
            dbus_message_iter_open_container(&levels[depth], DBUS_TYPE_STRUCT, nullptr, &levels[depth + 1]);
        }
        dbus_int32_t v = 5;
        dbus_message_iter_append_basic(&levels[depth], DBUS_TYPE_INT32, &v);
        for (; depth > 0; --depth) {
            dbus_message_iter_close_container(&levels[depth - 1], &levels[depth]);
        }
        c.add(m, ":1.20");
    }
    // Variants in variants.
    {
        DBusMessage* m = dbus_message_new_signal("/BMW/RueKoIC", kIface, "ric_variants");
        DBusMessageIter levels[12];
        dbus_message_iter_init_append(m, &levels[0]);
        for (int k = 0; k < 10; ++k) {
            dbus_message_iter_open_container(&levels[k], DBUS_TYPE_VARIANT, k == 9 ? "s" : "v", &levels[k + 1]);
        }
        appendText(&levels[10], "inside");
        for (int k = 10; k > 0; --k) {
            dbus_message_iter_close_container(&levels[k - 1], &levels[k]);
        }
        c.add(m, ":1.5");
    }
    // A line past 16 KB: 64 strings of 399 characters.
    {
        DBusMessage* m = dbus_message_new_signal("/BMW/RueKoIC", kIface, "ric_long");
        DBusMessageIter it, a;
        dbus_message_iter_init_append(m, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &a);
        for (int k = 0; k < 64; ++k) {
            appendText(&a, std::string(399, static_cast<char>('A' + k % 26)));
        }
        dbus_message_iter_close_container(&it, &a);
        for (int k = 0; k < 3; ++k) {
            appendText(&it, "after");
        }
        c.add(m, ":1.5");
    }
    {
        DBusMessage* m = dbus_message_new_signal("/BMW/RueKoIC", kIface, "ric_manyArgs");
        DBusMessageIter it;
        dbus_message_iter_init_append(m, &it);
        for (int k = 0; k < 45; ++k) {
            appendText(&it, std::string(390, 'z'));
        }
        c.add(m, ":1.5");
    }
    // Big-endian messages.
    c.addRaw(bigEndianMessage(DBUS_MESSAGE_TYPE_SIGNAL, 0x01020304, "ric_bigEndian", "ynqiuxtdsoas", true));
    c.addRaw(bigEndianMessage(DBUS_MESSAGE_TYPE_METHOD_CALL, 77, "ric_bigEndianCall", "a(ii)", false));
    // Random messages: calls, answers and signals with random arguments, and some on other interfaces.
    Rng r(0x5eed1234);
    for (int k = 0; k < 200; ++k) {
        DBusMessage* m;
        const uint32_t kind = r.below(10);
        char name[24];
        snprintf(name, sizeof(name), ":1.%u", 100 + r.below(30));
        if (kind < 5) {
            m = ricCall(r.below(3) ? "BMW.RueKoIC" : name, r.below(2) ? "ric_setData" : "ric_setResource");
        } else if (kind < 7) {
            m = dbus_message_new_signal("/BMW/RueKoIC", r.below(4) ? kIface : "BMW.Other", "ric_event");
        } else if (kind < 9) {
            DBusMessage* call = ricCall("BMW.RueKoIC", "ric_x");
            dbus_message_set_serial(call, r.next() | 1);
            dbus_message_set_sender(call, ":1.20");
            m = kind == 7 ? dbus_message_new_method_return(call)
                          : dbus_message_new_error(call, "BMW.Error.Random", "random error");
            dbus_message_unref(call);
        } else {
            m = dbus_message_new_method_call(name, "/x", "BMW.Other", "Other");
        }
        DBusMessageIter it;
        dbus_message_iter_init_append(m, &it);
        const uint32_t args = r.below(5);
        for (uint32_t a = 0; a < args; ++a) {
            appendRandom(&it, randomType(r, 0), r, 0);
        }
        c.add(m, r.below(5) ? ":1.20" : "BMW.RueKoIC");
    }
    return c;
}

// ---- the outputs -------------------------------------------------------------------------------------------------

std::string describeAll(const Corpus& c) {
    std::string out;
    for (size_t i = 0; i < c.read.size(); ++i) {
        g_fakeMs = 1000 + i * 37;
        out += describe(c.read[i], 1000);
        out += '\n';
    }
    return out;
}

std::string fileText(FILE* f) {
    fflush(f);
    rewind(f);
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        s.append(buf, n);
    }
    return s;
}

// What watch() writes for the corpus: its filter on every message, as the bus delivers them.
std::string watchAll(const Corpus& c, size_t maxBytes, DBusConnection* conn) {
    Watch w;
    w.file = tmpfile();
    check(w.file != nullptr, "a temporary file");
    w.t0 = 1000;
    w.bytes = 0;
    w.maxBytes = maxBytes;
    w.calls = w.answers = w.signals = w.dropped = 0;
    w.full = false;
    w.conn = conn;
    for (size_t i = 0; i < sizeof(kServers) / sizeof(kServers[0]); ++i) {
        w.answering.insert(kServers[i]);
    }
    for (size_t i = 0; i < c.read.size(); ++i) {
        g_fakeMs = 1000 + i * 37;
        check(onMessage(conn, c.read[i], &w) == DBUS_HANDLER_RESULT_HANDLED, "every message handled");
    }
    fprintf(w.file, "# counts: %u calls, %u answers, %u signals, %u bytes, %u dropped, full %d\n", w.calls, w.answers,
            w.signals, static_cast<unsigned>(w.bytes), w.dropped, w.full ? 1 : 0);
    fprintf(w.file, "# answers matched from:");
    for (std::set<std::string>::const_iterator it = w.answering.begin(); it != w.answering.end(); ++it) {
        fprintf(w.file, " %s", it->c_str());
    }
    fprintf(w.file, "\n");
    const std::string text = fileText(w.file);
    fclose(w.file);
    return text;
}

// What --selftest prints, with the log's path written FILE.
std::string selftestText() {
    fflush(stdout);
    FILE* capture = tmpfile();
    check(capture != nullptr, "a temporary file");
    const int saved = dup(STDOUT_FILENO);
    dup2(fileno(capture), STDOUT_FILENO);
    const int result = selftest();
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    check(result == 0, "evo-ric's self test passes");
    std::string text = fileText(capture);
    fclose(capture);
    const std::string marker = " recorded in ";
    const size_t at = text.find(marker);
    check(at != std::string::npos, "the self test names its log");
    const size_t end = text.find('\n', at);
    text.replace(at + marker.size(), end - at - marker.size(), "FILE");
    return text;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FIXTURES_DIR [--record]\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const bool recording = argc >= 3 && !strcmp(argv[2], "--record");
    const char* address = getenv("DBUS_SESSION_BUS_ADDRESS");
    check(address != nullptr, "a session bus (run it under dbus-run-session)");
    dbus_threads_init_default();
    DBusConnection* conn = openBus(address);
    check(conn != nullptr, "the session bus");
    const Corpus corpus = buildCorpus();
    const std::string files[][2] = {
        {"dbus/messages.bin", std::string(corpus.wire.begin(), corpus.wire.end())},
        {"evo_ric/describe.golden", describeAll(corpus)},
        {"evo_ric/watch.golden", watchAll(corpus, 8u << 20, conn)},
        {"evo_ric/watch-small.golden", watchAll(corpus, 3000, conn)},
        {"evo_ric/selftest.golden", selftestText()},
        {"evo_ric/usage.golden", kUsage},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        const std::string path = dir + "/" + files[i][0];
        if (recording) {
            std::ofstream out(path.c_str(), std::ios::binary);
            out << files[i][1];
            check(out.good(), "write a fixture");
            printf("recorded %s (%zu bytes)\n", path.c_str(), files[i][1].size());
            continue;
        }
        const std::string recorded = readFile(path);
        if (recorded != files[i][1]) {
            size_t at = 0;
            while (at < recorded.size() && at < files[i][1].size() && recorded[at] == files[i][1][at]) {
                ++at;
            }
            printf("FAIL ric golden: %s differs at byte %zu (recorded %zu bytes, the C++ gives %zu)\n", path.c_str(),
                   at, recorded.size(), files[i][1].size());
            ++failures;
        }
    }
    closeBus(conn);
    if (failures) {
        return 1;
    }
    if (!recording) {
        printf("PASS ric golden: %zu messages, their lines, the watch, the self test and the usage as recorded\n",
               corpus.read.size());
    }
    return 0;
}
