// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// evo-mfl : which MOST function each steering wheel button sends, on this car. READ ONLY, like the service's own
// steering wheel reader (platform/evo_knob.cpp): a shadow registration of ControlElementsMFL 0x15.02, which only
// copies its messages (iDrive keeps receiving everything), and plain reads (Get) of the functions libhmil6services
// names, which only open vmostd's filter for their status messages. Nothing is set, no file is written. Prints every
// message from 0x15.02 for the given number of seconds (default 60, at most 300), then deregisters.
// A developer tool, not part of a release (make dev-tools).
//   evo-mfl [seconds]
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "evo_knob.hpp"
#include "vmost_client.hpp"

namespace {

double now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double started;

struct Function {
    uint16_t id;
    const char* name;
};

// CPxyControlElementsMFL::init in libhmil6services.so, software 202306092158. The roller has its own functions;
// reading SearchSwitch1 alone does not open vmostd's filter for RotarySteps or RotaryPress.
const Function functions[] = {{evo::kMflTelephoneFkt, "Telephone"},
                              {evo::kVolumeMflFkt, "Volume"},
                              {0x511, "SearchSwitch1"},
                              {evo::kMflVoiceFkt, "Voice"},
                              {0x513, "SpecialFunction"},
                              {0xC30, "RotarySteps"},
                              {0xC31, "RotaryPress"}};

void onMessage(void* msg, void*) {
    evo::VmostMsgView m;
    if (!evo::parseVmostMsg(msg, m) || m.fblock != evo::kVolumeFblock || m.inst != evo::kVolumeMflInstance) {
        return;
    }
    const char* name = "other";
    for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
        if (functions[i].id == m.fkt) {
            name = functions[i].name;
            break;
        }
    }
    printf("[%8.3f] 15.%02x fkt=0x%03x %s op=0x%x len=%u:", now() - started, m.inst, m.fkt, name, (unsigned)m.op,
           m.len);
    for (unsigned i = 0; m.data && i < m.len && i < 24; ++i) {
        printf(" %02x", m.data[i]);
    }
    printf("\n");
    fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("evo-mfl [seconds, 1..300, default 60]: read-only steering-wheel capture");
        puts("Copies 15.02 status messages; plain Get reads for telephone, volume, search, voice, special function,");
        puts("RotarySteps (0xC30) and RotaryPress (0xC31). No source selection or button forwarding.");
        return 0;
    }
    long seconds = argc > 1 ? strtol(argv[1], nullptr, 10) : 60;
    if (seconds < 1 || seconds > 300) {
        fprintf(stderr, "seconds: 1 to 300\n");
        return 2;
    }
    evo::VmostClient vmost;
    if (vmost.load() != evo::VmostClient::kLoaded) {
        fprintf(stderr, "vmost: not available (%s)\n", vmost.error().c_str());
        return 1;
    }
    if (vmost.connect(onMessage, nullptr, 0) != 0) {
        fprintf(stderr, "vmost: connect refused\n");
        return 1;
    }
    started = now();
    const int reg = vmost.shadowRegister(evo::kVolumeFblock, evo::kVolumeMflInstance);
    printf("shadow 15.%02x: %s (%d)\n", evo::kVolumeMflInstance, reg == 0 ? "OK" : "refused", reg);
    if (reg != 0) {
        vmost.disconnect();
        return 1;
    }
    for (unsigned i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
        evo::VmostMsgOut out;
        static const uint8_t dummy[4] = {0, 0, 0, 0};
        evo::buildVmostGet(out, evo::kVolumeFblock, evo::kVolumeMflInstance, functions[i].id, dummy);
        const int rc = vmost.send(&out);
        printf("read (Get) 15.%02x fkt=0x%03x %s: %s (%d)\n", evo::kVolumeMflInstance, functions[i].id,
               functions[i].name, rc == 0 ? "sent" : "not sent", rc);
    }
    printf("listening %ld s: press the steering wheel buttons one at a time\n", seconds);
    fflush(stdout);
    int fd = -1;
    if (vmost.fileDescriptorGet(&fd) != 0 || fd < 0) {
        fprintf(stderr, "vmost: no file descriptor\n");
    }
    while (now() - started < seconds) {
        pollfd p = {fd, POLLIN, 0};
        if (fd >= 0 && poll(&p, 1, 200) > 0) {
            vmost.processMessages();
        } else if (fd < 0) {
            struct timespec ts = {0, 200000000};
            nanosleep(&ts, nullptr);
        }
    }
    vmost.shadowDeregister(evo::kVolumeFblock, evo::kVolumeMflInstance);
    vmost.disconnect();
    printf("done\n");
    return 0;
}
