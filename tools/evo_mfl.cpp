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

void onMessage(void* msg, void*) {
    evo::VmostMsgView m;
    if (!evo::parseVmostMsg(msg, m) || m.fblock != evo::kVolumeFblock || m.inst != evo::kVolumeMflInstance) {
        return;
    }
    printf("[%8.3f] 15.%02x fkt=0x%03x op=0x%x len=%u:", now() - started, m.inst, m.fkt, (unsigned)m.op, m.len);
    for (unsigned i = 0; m.data && i < m.len && i < 24; ++i) {
        printf(" %02x", m.data[i]);
    }
    printf("\n");
    fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
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
    static const uint16_t functions[] = {evo::kMflTelephoneFkt, evo::kVolumeMflFkt, 0x511, 0x512, 0x513};
    for (unsigned i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
        evo::VmostMsgOut out;
        static const uint8_t dummy[4] = {0, 0, 0, 0};
        evo::buildVmostGet(out, evo::kVolumeFblock, evo::kVolumeMflInstance, functions[i], dummy);
        const int rc = vmost.send(&out);
        printf("read (Get) 15.%02x fkt=0x%03x: %s (%d)\n", evo::kVolumeMflInstance, functions[i],
               rc == 0 ? "sent" : "not sent", rc);
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
