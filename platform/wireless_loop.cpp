// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The Bluetooth half of a wireless start (see wireless_loop.hpp).
#include "wireless_loop.hpp"

#include <unistd.h>

#include "alps_spp.hpp"
#include "evo_log.hpp"

namespace evo {

namespace {
const int kPeekMs = 300;  // a MyBMW link speaks first: nothing is written into it

// A link the phone opened again while its session runs over the Wi-Fi. On the reference car, BMW's Bluetooth manager
// opens a link to the phone's My BMW app on its own and closes it about 10 s later, and Android Auto's link closes with
// it (14 November 2026). The phone opens its link again at once and waits for the car to speak first. Left silent, it
// closed the link after exactly 12 s and opened it again, over and over, and twice the session ended with one of those
// closes. So it gets the handshake again, with the Wi-Fi it is already on. Should it open a second TCP connection,
// the TCP server refuses it while the session runs.
void answerDuringSession(AlpsSppLink& bt, int fd, const aa::ApInfo& ap, uint16_t port, WirelessHost& host) {
    ELOG("WirelessLoop", "Bluetooth link back while Android Auto runs on the Wi-Fi: the same Wi-Fi offered again");
    const uint32_t sinceMs = monoMs();
    aa::WirelessSetup setup(fd, ap, port, kPeekMs);
    const bool ok = !host.stopRequested() && setup.handshake(10000);
    ELOG("WirelessLoop", ok ? "the phone took the same Wi-Fi again: the link is kept"
                            : "the phone did not take the Wi-Fi again on this link");
    if (ok) {
        while (!host.stopRequested() && bt.linkUp() && !host.cableBusy() && setup.serve(1000)) {
        }
    }
    ELOG("WirelessLoop", "that Bluetooth link ended after %u ms", monoMs() - sinceMs);
    bt.closeLink();  // owns fd; no close(fd) in the caller
}
}  // namespace

void runWirelessLoop(AlpsSppLink& bt, const aa::ApInfo& ap, uint16_t port, WirelessHost& host) {
    while (!host.stopRequested() && bt.running()) {
        // Lifecycle guard: a returning SPP must not restart a TCP projection being prepared, nor take the phone from
        // the cable. During a session on the Wi-Fi, a returning link is answered (answerDuringSession).
        if (!host.wifiSessionRunning() && (host.projectionBusy() || host.cableBusy())) {
            usleep(100000);
            continue;
        }
        int fd = bt.waitLink(250);
        if (fd < 0) {
            continue;
        }
        if (host.wifiSessionRunning()) {
            answerDuringSession(bt, fd, ap, port, host);
            continue;
        }
        // TCP may have arrived while waitLink was sleeping, or the cable taken the phone. Hold this local link
        // without a new WifiStartRequest until that ends (or Bluetooth/stop interrupts us).
        while ((host.projectionBusy() || host.cableBusy()) && !host.wifiSessionRunning() && !host.stopRequested() &&
               bt.linkUp()) {
            usleep(100000);
        }
        if (host.stopRequested() || !bt.linkUp()) {
            bt.closeLink();
            continue;
        }
        if (host.wifiSessionRunning()) {
            answerDuringSession(bt, fd, ap, port, host);
            continue;
        }
        host.sppLinkOpened();
        ELOG("WirelessLoop", "SPP link open; starting the WPP exchange");
        // Credentials are captured from the live AP at startup, never from the saved car report.
        aa::WirelessSetup setup(fd, ap, port, kPeekMs);
        bool ok = !host.stopRequested() && setup.handshake(10000);
        host.handshakeEnded(setup.spokeWireless(), ok);
        if (ok) {
            ELOG("WirelessLoop", "WPP accepted; waiting for TCP on port %u", port);
            while (!host.stopRequested() && bt.linkUp() && !host.cableBusy() && setup.serve(1000)) {
            }
            if (host.cableBusy()) {
                ELOG("WirelessLoop", "the cable has the phone: Wi-Fi start ended");
            }
        }
        bt.closeLink();  // owns fd; no close(fd) in the caller
    }
}

}  // namespace evo
