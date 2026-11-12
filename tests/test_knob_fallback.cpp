// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The iDrive controller on a car that speaks the older CCS protocol: the reads that open vmostd's filter for it go out
// only when the CCS35up reads brought no controller message, and once. Also the picture path's statistics.

// Uses tests/fake_vmostclient.c in place of the HU's libvmostclient.so.1 (no car).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#include "evo_knob.hpp"
#include "evo_video.hpp"
#include "vmost_client.hpp"

#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            return 1;                                            \
        }                                                        \
    } while (0)

static std::string readAll(const char* p) {
    std::string s;
    FILE* f = fopen(p, "r");
    if (!f) {
        return s;
    }
    char b[512];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) {
        s.append(b, n);
    }
    fclose(f);
    return s;
}

static size_t count(const std::string& text, const char* what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

static int g_turns;
static void onEvent(const aa::CcsEvent& e, void*) {
    if (e.kind == 2) {
        g_turns += e.delta;
    }
}

// One run of the controller reader against the fake library; returns what it sent.
static std::string run(const char* controller, const char* log, unsigned ms) {
    unlink(log);
    if (controller) {
        setenv("FAKE_CCS", controller, 1);
    } else {
        unsetenv("FAKE_CCS");
    }
    g_turns = 0;
    evo::EvoKnob knob;
    if (!knob.start(evo::INPUT_FORWARD, onEvent, nullptr)) {
        return "start failed";
    }
    knob.setOwned(true);  // the Android Auto picture shows: keys are forwarded
    usleep(ms * 1000);
    knob.stop();
    return "\n" + readAll(log);  // every line then starts after a line break, the first one too
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test-knob-fallback <fake-libvmostclient.so>\n");
        return 2;
    }
    // When the older protocol's reads are due.
    CHECK(!evo::ccsFallbackDue(false, 0, 1000, 1000 + evo::kCcsFallbackMs - 1));  // still waiting for an answer
    CHECK(evo::ccsFallbackDue(false, 0, 1000, 1000 + evo::kCcsFallbackMs));
    CHECK(!evo::ccsFallbackDue(false, 1, 1000, 60000));                      // the controller answered: never
    CHECK(!evo::ccsFallbackDue(true, 0, 1000, 60000));                       // sent once only
    CHECK(evo::ccsFallbackDue(false, 0, 0xFFFFFF00u, evo::kCcsFallbackMs));  // the clock wrapped in between

    char dir[] = "/tmp/entry-knob-test-XXXXXX";
    CHECK(mkdtemp(dir));
    const std::string sock = std::string(dir) + "/vmost_sockstream", log = std::string(dir) + "/sent.log";
    setenv("FAKE_VMOST_LOG", log.c_str(), 1);  // read by the fake library
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(s >= 0);
    sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sock.c_str(), sizeof(a.sun_path) - 1);
    CHECK(bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    evo::setVmostForTest(sock.c_str(), argv[1]);

    const char* const kNew[] = {"send addr=ffff e5.03.c46 op=1 len=0", "send addr=ffff e5.03.c47 op=1 len=0",
                                "send addr=ffff e5.03.c4f op=1 len=0"};
    const char* const kOld[] = {"send addr=ffff e5.01.c10 op=1 len=0", "send addr=ffff e5.01.c20 op=1 len=0"};
    const unsigned wait = evo::kCcsFallbackMs + 700;

    // A CCS35up controller answers: nothing more than before is sent, however long the service runs.
    std::string sent = run("35up", log.c_str(), wait);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(count(sent, kNew[i]) == 1);
    }
    // + volume (2), phone button, voice button, volume level
    CHECK(count(sent, "e5.01.") == 0 && count(sent, "op=1") == 3 + 5);
    CHECK(count(sent, "send addr=ffff 15.02.512 op=1 len=0") == 1);
    CHECK(count(sent, "\nregister e5.") == 3);  // instances 01, 03 and 05, as before

    // No answer and it has not been long enough: nothing yet.
    sent = run(nullptr, log.c_str(), evo::kCcsFallbackMs / 3);
    CHECK(count(sent, "e5.03.") == 3 && count(sent, "e5.01.") == 0);

    // No answer at all: the older protocol's two reads, once each, and only reads.
    sent = run(nullptr, log.c_str(), wait);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(count(sent, kOld[i]) == 1);
    }
    CHECK(count(sent, "e5.03.") == 3 && count(sent, "e5.01.") == 2 && count(sent, "op=1") == 5 + 5);
    CHECK(count(sent, "send ") == count(sent, "op=1"));  // nothing but reads was ever sent
    // From then on the other known controller instances are copied too, and given back at the end.
    const char* const kMore[] = {"register e5.02\n", "register e5.10\n", "register e5.12\n", "register e5.14\n"};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(count(sent, (std::string("\n") + kMore[i]).c_str()) == 1);
        CHECK(count(sent, (std::string("\nde") + kMore[i]).c_str()) == 1);
    }
    CHECK(count(sent, "\nregister e5.") == 7);

    // The older controller: its messages now arrive and its detent reaches Android Auto.
    sent = run("old", log.c_str(), wait);
    CHECK(count(sent, kOld[0]) == 1 && count(sent, kOld[1]) == 1 && g_turns == 1);
    // Everything registered was deregistered.
    CHECK(count(sent, "\nregister ") == count(sent, "\nderegister "));

    // The picture path's statistics: counts, the longest gap (also across two stretches), and when a line is due.
    evo::VideoStats v;
    CHECK(!v.due(60000));  // no frame: no line
    v.reset(1000);
    v.frame(1000);
    v.frame(1040);
    v.frame(1400);
    CHECK(v.fromPhone == 3 && v.maxGapMs == 360 && !v.due(1000 + evo::kVideoStatsMs - 1));
    CHECK(v.due(1000 + evo::kVideoStatsMs));
    v.reset(11000);
    CHECK(v.fromPhone == 0 && v.maxGapMs == 0 && !v.due(30000));
    v.frame(13400);
    CHECK(v.fromPhone == 1 && v.maxGapMs == 12000);  // since the last frame of the stretch before

    unlink(log.c_str());
    close(s);
    unlink(sock.c_str());
    rmdir(dir);
    puts(
        "PASS controller: older CCS reads (E5.01 C10/C20) only without any answer to the CCS35up reads, once, reads "
        "only; older controller's detent reaches Android Auto; picture statistics");
    return 0;
}
