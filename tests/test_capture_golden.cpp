// Copyright (c) 2026 CallMeLewis
// The C++ evo-capture's outputs, recorded for its Rust port (rust/evo-capture) and checked against what was recorded:
//   corpus.pcap     a fixed set of IPv4 packets, written by the tool's own pcap writer
//   summary.golden  the tool's summary of those packets
//   selftest.golden what evo-capture --selftest prints
// rust/evo-capture/tests/golden.rs reads the same three files and must produce the same bytes. A record's
// microseconds field names the interface its packet came in on: kInterfaces[usec].
//   test-capture-golden DIR            check that the C++ still gives what DIR holds (make test-host)
//   test-capture-golden DIR --record   write them again (only when the C++'s behaviour is meant to change)
//   test-capture-golden --summarize FILE.pcap
//                                      the C++ summary of a pcap recorded in the car, every packet on interface "car",
//                                      to compare with rust/evo-capture's example summarize (docs/rust-port.md)
#define main evo_capture_main
#include "../tools/evo_capture.cpp"
#undef main

#include <fstream>
#include <iterator>

namespace {

const char* const kInterfaces[] = {"vbr0.73", "eth0", "wlan0"};
const unsigned kEpoch = 1790000000;  // the records' first second

struct Packet {
    unsigned interface;
    std::vector<uint8_t> bytes;
};

typedef std::vector<uint8_t> Bytes;

Bytes cat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void put16(Bytes& v, size_t at, unsigned value) {
    v[at] = static_cast<uint8_t>(value >> 8);
    v[at + 1] = static_cast<uint8_t>(value);
}

void put32(Bytes& v, size_t at, uint32_t value) {
    put16(v, at, value >> 16);
    put16(v, at + 2, value & 0xffff);
}

// An IPv4 TCP segment with a data offset of `words` 32-bit words.
Bytes tcp(const uint8_t src[4], unsigned sp, const uint8_t dst[4], unsigned dp, const Bytes& payload,
          unsigned words = 5) {
    Bytes v(20 + words * 4 + payload.size(), 0);
    v[0] = 0x45;
    put16(v, 2, v.size());
    v[8] = 64;
    v[9] = 6;
    memcpy(&v[12], src, 4);
    memcpy(&v[16], dst, 4);
    put16(v, 20, sp);
    put16(v, 22, dp);
    v[32] = static_cast<uint8_t>(words << 4);
    if (!payload.empty()) {
        memcpy(&v[20 + words * 4], payload.data(), payload.size());
    }
    return v;
}

// A SOME/IP message with every header field chosen.
Bytes rawSomeip(unsigned svc, unsigned method, uint32_t len, unsigned proto, unsigned iface, unsigned type,
                const Bytes& body) {
    Bytes v(16, 0);
    put16(v, 0, svc);
    put16(v, 2, method);
    put32(v, 4, len);
    v[12] = static_cast<uint8_t>(proto);
    v[13] = static_cast<uint8_t>(iface);
    v[14] = static_cast<uint8_t>(type);
    return cat(v, body);
}

// One SOME/IP-SD entry: type, first and second option runs, service, instance, major, TTL, then minor or eventgroup.
Bytes sdEntry(unsigned type, unsigned i1, unsigned i2, unsigned n1, unsigned n2, unsigned svc, unsigned inst,
              unsigned major, uint32_t ttl, uint32_t last) {
    Bytes e(16, 0);
    e[0] = static_cast<uint8_t>(type);
    e[1] = static_cast<uint8_t>(i1);
    e[2] = static_cast<uint8_t>(i2);
    e[3] = static_cast<uint8_t>(n1 << 4 | n2);
    put16(e, 4, svc);
    put16(e, 6, inst);
    put32(e, 8, major << 24 | (ttl & 0xffffff));
    put32(e, 12, last);
    return e;
}

// An IPv4 endpoint or multicast option (length 9).
Bytes endpoint(unsigned type, const uint8_t ip[4], unsigned proto, unsigned port) {
    Bytes o(12, 0);
    put16(o, 0, 9);
    o[2] = static_cast<uint8_t>(type);
    memcpy(&o[4], ip, 4);
    o[9] = static_cast<uint8_t>(proto);
    put16(o, 10, port);
    return o;
}

Bytes option(unsigned type, unsigned length) {
    Bytes o(3 + length, 0x11);
    put16(o, 0, length);
    o[2] = static_cast<uint8_t>(type);
    return o;
}

// An SD payload: flags, the entries with their length, the options with theirs.
Bytes sdPayload(const Bytes& entries, const Bytes& options) {
    Bytes v(8, 0);
    v[0] = 0xc0;
    put32(v, 4, entries.size());
    v = cat(v, entries);
    Bytes olen(4, 0);
    put32(olen, 0, options.size());
    return cat(cat(v, olen), options);
}

Bytes sdMessage(const Bytes& payload) {
    return someip(0xffff, 0x8100, 0x02, payload);
}

// Reproducible pseudo-random bytes, so that --record writes the same corpus again.
struct Lcg {
    uint32_t x;
    explicit Lcg(uint32_t seed) : x(seed) {}
    unsigned next(unsigned below) {
        x = x * 1103515245u + 12345u;
        return (x >> 8) % below;
    }
};

std::vector<Packet> corpus() {
    const uint8_t hu[4] = {160, 48, 199, 119}, kombi[4] = {160, 48, 199, 20}, mc[4] = {224, 244, 224, 245};
    const uint8_t hud[4] = {160, 48, 199, 30}, phone[4] = {192, 168, 10, 4};
    std::vector<Packet> out;
    // ---- what --selftest feeds -------------------------------------------------------------------------------------
    Bytes sd(4 + 4 + 16 + 4 + 12, 0);
    sd[0] = 0xc0;
    sd[7] = 16;
    sd[8] = 0x01;
    sd[11] = 0x10;
    sd[12] = 0x12;
    sd[13] = 0x34;
    sd[15] = 1;
    sd[16] = 1;
    sd[19] = 3;
    sd[27] = 12;
    sd[29] = 9;
    sd[30] = 0x04;
    memcpy(&sd[32], hu, 4);
    sd[37] = 17;
    sd[38] = 0x77;
    sd[39] = 0x5e;
    out.push_back(Packet{0, udp(hu, 30490, mc, 30490, sdMessage(sd))});
    out.push_back(Packet{
        0, udp(hu, 30558, kombi, 30501,
               cat(someip(0x1234, 0x8001, 0x02, Bytes(5, 0xab)), someip(0x1234, 0x8001, 0x02, Bytes(3, 0xcd))))});
    out.push_back(Packet{0, udp(kombi, 5000, hu, 5001, Bytes(20, 0x55))});
    Bytes bad = sd;
    bad[4] = bad[5] = bad[6] = 0xff;
    bad[7] = 0xf8;
    out.push_back(Packet{1, udp(hu, 30490, mc, 30490, sdMessage(bad))});
    bad = sd;
    bad[24] = bad[25] = bad[26] = 0xff;
    bad[27] = 0xfc;
    out.push_back(Packet{1, udp(hu, 30490, mc, 30490, sdMessage(bad))});

    // ---- SOME/IP-SD: every entry type, every option kind, indexes past the options ---------------------------------
    Bytes entries;
    entries = cat(entries, sdEntry(0x00, 0, 0, 0, 0, 0xb006, 0xffff, 0xff, 3, 0xffffffffu));  // FIND
    entries = cat(entries, sdEntry(0x01, 0, 0, 2, 0, 0xb006, 0x0001, 1, 3, 0));               // OFFER, 2 endpoints
    entries = cat(entries, sdEntry(0x01, 1, 0, 1, 0, 0xb006, 0x0001, 1, 0, 7));               // STOP_OFFER
    entries = cat(entries, sdEntry(0x06, 0, 2, 1, 1, 0xb006, 0x0001, 1, 3, 0x0001));          // SUBSCRIBE, 2 runs
    entries = cat(entries, sdEntry(0x06, 0, 0, 0, 0, 0xb006, 0x0001, 1, 0, 0x0001));          // STOP_SUBSCRIBE
    entries = cat(entries, sdEntry(0x07, 3, 4, 1, 2, 0xb006, 0x0001, 1, 3, 0x0002));          // SUBSCRIBE_ACK
    entries = cat(entries, sdEntry(0x07, 0, 0, 0, 0, 0xb006, 0x0001, 1, 0, 0x0002));          // SUBSCRIBE_NACK
    entries = cat(entries, sdEntry(0x42, 0, 0, 1, 0, 0x0102, 0x0304, 5, 3, 6));               // unknown type
    entries = cat(entries, sdEntry(0x01, 9, 14, 3, 15, 0xb006, 0x0002, 1, 3, 1));             // past the options
    Bytes options = endpoint(0x04, hu, 6, 30501);                                             // tcp
    options = cat(options, endpoint(0x14, mc, 17, 30490));
    options = cat(options, endpoint(0x04, kombi, 0x99, 1));  // unknown L4 protocol
    options = cat(options, option(0x01, 5));                 // configuration
    options = cat(options, option(0x04, 5));                 // endpoint type with the wrong length
    out.push_back(Packet{0, udp(hu, 30490, mc, 30490, sdMessage(sdPayload(entries, options)))});
    // The same once more from the cluster, then options whose last one runs past their length.
    out.push_back(Packet{2, udp(kombi, 30490, mc, 30490, sdMessage(sdPayload(entries, options)))});
    Bytes cut = cat(endpoint(0x04, hud, 17, 30600), option(0x01, 40));
    cut.resize(cut.size() - 20);
    out.push_back(Packet{0, udp(hud, 30490, mc, 30490, sdMessage(sdPayload(entries, cut)))});
    // Entries of 20 bytes: one entry, the rest ignored. Then SD bodies too short for a header or for the lengths.
    Bytes odd = sdEntry(0x01, 0, 0, 1, 0, 0xb007, 0x0001, 2, 3, 9);
    odd.resize(20, 0x22);
    out.push_back(Packet{0, udp(hu, 30490, mc, 30490, sdMessage(sdPayload(odd, endpoint(0x04, hu, 17, 1))))});
    out.push_back(Packet{0, udp(hu, 30490, mc, 30490, sdMessage(Bytes(7, 0)))});
    out.push_back(Packet{0, udp(hu, 30490, mc, 30490, sdMessage(Bytes(11, 0)))});
    Bytes noOptions(12, 0);  // header and options length, no entries
    out.push_back(Packet{0, udp(hu, 30490, mc, 30490, sdMessage(noOptions))});

    // ---- SOME/IP headers: every type, the TP flag, interface versions, refused headers -----------------------------
    const unsigned types[] = {0x00, 0x01, 0x02, 0x80, 0x81, 0x22, 0x23, 0x40, 0xa0, 0xa1};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        Bytes body(i * 7, static_cast<uint8_t>(i));
        out.push_back(Packet{static_cast<unsigned>(i % 3),
                             udp(hu, 40000 + i, kombi, 30501,
                                 rawSomeip(0xb006, 0x0100 + i, 8 + body.size(), 1, i % 4, types[i], body))});
    }
    out.push_back(Packet{0, udp(hu, 1, kombi, 2, rawSomeip(0xb006, 1, 8, 2, 1, 0, Bytes()))});             // protocol 2
    out.push_back(Packet{0, udp(hu, 1, kombi, 2, rawSomeip(0xb006, 1, 7, 1, 1, 0, Bytes(8, 0)))});         // length 7
    out.push_back(Packet{0, udp(hu, 1, kombi, 2, rawSomeip(0xb006, 1, 0x100001, 1, 1, 0, Bytes()))});      // too long
    out.push_back(Packet{0, udp(hu, 1, kombi, 2, rawSomeip(0xb006, 1, 0x100000, 1, 1, 0, Bytes(4, 1)))});  // cut UDP
    // A good message, then garbage: the first counts, the rest stops the datagram.
    out.push_back(Packet{0, udp(hu, 1, kombi, 2, cat(someip(0xb006, 2, 0x02, Bytes(2, 9)), Bytes(15, 0xee)))});
    // Samples: an empty body, exactly 96 bytes, and more than 96.
    out.push_back(Packet{1, udp(hu, 3, kombi, 4, someip(0xb006, 0x0200, 0x02, Bytes()))});
    out.push_back(Packet{1, udp(hu, 3, kombi, 4, someip(0xb006, 0x0201, 0x02, Bytes(96, 0x5a)))});
    out.push_back(Packet{1, udp(hu, 3, kombi, 4, someip(0xb006, 0x0202, 0x02, Bytes(120, 0xa5)))});
    // One pair from eight flows and with five payloads: six flows and three samples are kept.
    for (unsigned i = 0; i < 8; ++i) {
        Bytes body(3, static_cast<uint8_t>(0x30 + i));
        out.push_back(Packet{i % 3, udp(hu, 50000 + i, kombi, 30501, someip(0xb006, 0x8005, 0x02, body))});
    }

    // ---- TCP --------------------------------------------------------------------------------------------------------
    out.push_back(
        Packet{2, tcp(phone, 41000, hu, 30509,
                      cat(someip(0xb006, 0x0010, 0x00, Bytes(4, 1)), someip(0xb006, 0x0010, 0x80, Bytes(6, 2))))});
    Bytes partial = cat(someip(0xb006, 0x0011, 0x00, Bytes(4, 3)), someip(0xb006, 0x0012, 0x00, Bytes(200, 4)));
    partial.resize(partial.size() - 150);  // the second message continues in the next segment
    out.push_back(Packet{2, tcp(phone, 41000, hu, 30509, partial)});
    out.push_back(Packet{2, tcp(phone, 41000, hu, 30509, someip(0xb006, 0x0013, 0x00, Bytes(2, 5)), 8)});  // options
    out.push_back(Packet{0, tcp(phone, 22, hu, 50123, someip(0xb006, 0x0014, 0x00, Bytes(2, 6)))});        // SSH
    out.push_back(Packet{0, tcp(phone, 50123, hu, 22, someip(0xb006, 0x0014, 0x00, Bytes(2, 6)))});
    Bytes shortOffset = tcp(phone, 41001, hu, 30509, someip(0xb006, 0x0015, 0x00, Bytes(2, 7)));
    shortOffset[32] = 4 << 4;  // data offset 16 bytes
    out.push_back(Packet{0, shortOffset});
    Bytes longOffset = tcp(phone, 41001, hu, 30509, Bytes());
    longOffset[32] = 15 << 4;  // data offset 60 bytes, past the segment
    out.push_back(Packet{0, longOffset});
    Bytes shortTcp = tcp(phone, 41001, hu, 30509, Bytes());
    shortTcp.resize(30);  // 10 bytes of TCP header
    put16(shortTcp, 2, 30);
    out.push_back(Packet{0, shortTcp});

    // ---- IPv4 headers -----------------------------------------------------------------------------------------------
    out.push_back(Packet{0, Bytes(10, 0x45)});  // shorter than a header
    Bytes v6(40, 0);
    v6[0] = 0x60;
    out.push_back(Packet{1, v6});
    Bytes p = udp(hu, 6, kombi, 7, someip(0xb006, 0x0300, 0x02, Bytes(4, 8)));
    Bytes q = p;
    q[0] = 0x44;  // IHL 16 bytes
    out.push_back(Packet{0, q});
    q = p;
    put16(q, 2, 12);  // total length shorter than the header
    out.push_back(Packet{0, q});
    q = cat(p, someip(0xb006, 0x0301, 0x02, Bytes(4, 9)));  // trailing bytes past the total length are not read
    out.push_back(Packet{0, q});
    q = p;
    put16(q, 6, 0x0001);  // a later fragment
    out.push_back(Packet{0, q});
    q = p;
    put16(q, 6, 0x2000);  // more fragments, but this is the first
    out.push_back(Packet{0, q});
    q = p;
    q[9] = 1;  // ICMP
    out.push_back(Packet{0, q});
    q = Bytes(p.begin(), p.begin() + 26);  // UDP header cut short
    put16(q, 2, 26);
    out.push_back(Packet{0, q});
    q = Bytes(p.begin(), p.begin() + 20);
    q[0] = 0x46;  // IHL 24 bytes
    q.resize(24, 0);
    q = cat(q, Bytes(p.begin() + 20, p.end()));
    put16(q, 2, q.size());
    out.push_back(Packet{0, q});
    q = Bytes(p.begin(), p.begin() + 40);
    q[0] = 0x4f;       // IHL 60 bytes, longer than the packet
    put16(q, 2, 100);  // total length past the packet
    out.push_back(Packet{0, q});

    // ---- reproducible noise: mostly plausible headers with random fields -------------------------------------------
    Lcg rng(0x0aa0e17u);
    for (unsigned i = 0; i < 320; ++i) {
        Bytes v(rng.next(220), 0);
        for (size_t k = 0; k < v.size(); ++k) {
            v[k] = static_cast<uint8_t>(rng.next(256));
        }
        if (v.size() >= 20 && rng.next(8) != 0) {
            v[0] = rng.next(6) ? 0x45 : static_cast<uint8_t>(0x40 | rng.next(16));
            put16(v, 2, rng.next(6) ? v.size() : rng.next(400));
            put16(v, 6, rng.next(5) ? 0 : rng.next(0x10000));
            const unsigned protos[] = {17, 17, 6, 6, 1, 47};
            v[9] = static_cast<uint8_t>(protos[rng.next(6)]);
            size_t ihl = (v[0] & 15) * 4u;
            size_t l4 = ihl + (v[9] == 6 ? 4 * (v.size() > ihl + 12 ? (v[ihl + 12] >> 4) : 5) : 8);
            if (v[9] == 6 && v.size() > ihl + 12 && rng.next(4)) {
                v[ihl + 12] = 5 << 4;
                l4 = ihl + 20;
            }
            if (l4 + 16 <= v.size() && rng.next(4)) {  // a SOME/IP header, often a plausible one
                put16(v, l4, rng.next(3) ? 0xb006 : 0xffff);
                put16(v, l4 + 2, rng.next(3) ? 0x8001 : 0x8100);
                put32(v, l4 + 4, rng.next(3) ? v.size() - l4 - 8 : rng.next(300));
                v[l4 + 12] = rng.next(5) ? 1 : static_cast<uint8_t>(rng.next(3));
                v[l4 + 14] = static_cast<uint8_t>(types[rng.next(10)]);
            }
        }
        out.push_back(Packet{rng.next(3), v});
    }
    return out;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string readStream(FILE* f) {
    fflush(f);
    std::string text;
    rewind(f);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    fclose(f);
    return text;
}

// The pcap the tool's own writer makes of the corpus.
std::string corpusPcap(const std::vector<Packet>& packets) {
    char path[] = "/tmp/test-capture-golden-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return std::string();
    }
    close(fd);
    unlink(path);  // Pcap::open creates it, and refuses one that exists
    Pcap pcap;
    if (!pcap.open(path)) {
        return std::string();
    }
    for (size_t i = 0; i < packets.size(); ++i) {
        struct timeval tv;
        tv.tv_sec = kEpoch + i;
        tv.tv_usec = packets[i].interface;
        pcap.add(tv, packets[i].bytes.data(), packets[i].bytes.size(), packets[i].bytes.size());
    }
    pcap.close();
    std::string bytes = readFile(path);
    unlink(path);
    return bytes;
}

std::string corpusSummary(const std::vector<Packet>& packets) {
    Summary s;
    for (size_t i = 0; i < packets.size(); ++i) {
        s.addIpv4(kInterfaces[packets[i].interface], packets[i].bytes.data(), packets[i].bytes.size());
    }
    FILE* f = tmpfile();
    s.print(f);
    return readStream(f);
}

// What selftest() prints on stdout, and whether it passed.
std::string selftestOutput(bool& passed) {
    FILE* f = tmpfile();
    fflush(stdout);
    int saved = dup(1);
    dup2(fileno(f), 1);
    passed = selftest() == 0;
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    return readStream(f);
}

uint32_t le32(const std::string& s, size_t at) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3])) << 24;
}

// The C++ summary of every packet of a pcap evo-capture wrote (little-endian, raw IPv4).
int summarize(const char* path) {
    const std::string file = readFile(path);
    if (file.size() < 24 || le32(file, 0) != 0xa1b2c3d4u || le32(file, 20) != 101) {
        fprintf(stderr, "%s: not a pcap of raw IPv4 from evo-capture\n", path);
        return 2;
    }
    Summary s;
    for (size_t at = 24; at < file.size();) {
        if (file.size() - at < 16 || le32(file, at + 8) > file.size() - at - 16) {
            fprintf(stderr, "%s: cut short at byte %zu\n", path, at);
            return 2;
        }
        const size_t caplen = le32(file, at + 8);
        s.addIpv4("car", reinterpret_cast<const uint8_t*>(file.data() + at + 16), caplen);
        at += 16 + caplen;
    }
    s.print(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FIXTURE_DIR [--record] | --summarize FILE.pcap\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--summarize")) {
        return argc == 3 ? summarize(argv[2]) : 2;
    }
    const std::string dir = argv[1];
    const bool recording = argc >= 3 && !strcmp(argv[2], "--record");
    const std::vector<Packet> packets = corpus();
    bool passed = false;
    const std::string files[3][2] = {{"corpus.pcap", corpusPcap(packets)},
                                     {"summary.golden", corpusSummary(packets)},
                                     {"selftest.golden", selftestOutput(passed)}};
    if (!passed || files[0][1].empty()) {
        printf("FAIL capture golden: the C++ self test or pcap writer failed\n");
        return 1;
    }
    int failures = 0;
    for (size_t i = 0; i < 3; ++i) {
        const std::string path = dir + "/" + files[i][0];
        if (recording) {
            std::ofstream(path.c_str(), std::ios::binary) << files[i][1];
            printf("recorded %s (%zu bytes)\n", path.c_str(), files[i][1].size());
            continue;
        }
        const std::string want = readFile(path);
        if (want != files[i][1]) {
            size_t at = 0;
            while (at < want.size() && at < files[i][1].size() && want[at] == files[i][1][at]) {
                ++at;
            }
            printf("FAIL capture golden: %s differs at byte %zu (recorded %zu bytes, the C++ gives %zu)\n",
                   path.c_str(), at, want.size(), files[i][1].size());
            ++failures;
        }
    }
    if (!recording && !failures) {
        printf("PASS capture golden: %zu packets, their pcap, summary and the self test as recorded\n", packets.size());
    }
    return failures ? 1 : 0;
}
