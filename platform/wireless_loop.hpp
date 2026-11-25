// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The Bluetooth half of a wireless start: each phone link on the SPP slot gets the Wi-Fi handshake, one at a time.
#ifndef AA_ENTRYNAV_PLATFORM_WIRELESS_LOOP_HPP
#define AA_ENTRYNAV_PLATFORM_WIRELESS_LOOP_HPP

#include <stdint.h>

#include "wireless_setup.hpp"

namespace evo {

class AlpsSppLink;

// What the loop asks of the service, from the loop's own thread.
class WirelessHost {
public:
    virtual bool stopRequested() = 0;
    // A phone connection is being prepared or its Android Auto session runs: no new handshake meanwhile.
    virtual bool projectionBusy() = 0;
    // The cable is connecting a phone or carries its session: the phone is not sent onto the Wi-Fi meanwhile, and a
    // Wi-Fi start already given is ended. Seen on the car on 11 November 2026: with both going, the phone accepted
    // the Wi-Fi a few seconds before the cable session started, then dropped the cable for the Wi-Fi 7 s later, with
    // its Bluetooth links to the car reconnecting around each switch.
    virtual bool cableBusy() = 0;
    // An Android Auto session runs over the Wi-Fi: not being prepared, and not on the cable.
    virtual bool wifiSessionRunning() = 0;
    virtual void sppLinkOpened() = 0;
    // Just before a phone is told the access point: the next phone that joins it is offered no U-APSD
    // (platform/ap_uapsd.hpp). Not called for a phone already on the Wi-Fi (answerDuringSession), which would gain
    // nothing until it joins again.
    virtual void prepareAccessPoint() = 0;
    // A handshake ended: the phone spoke the wireless protocol, it accepted the Wi-Fi.
    virtual void handshakeEnded(bool spoke, bool accepted) = 0;
    // Sessions over the Wi-Fi lost so far: ended without the phone's ByeByeRequest (silent phone, broken connection,
    // stuck decoder), and not replaced on purpose by the phone's new connection. A count, so that the loop sees a loss
    // that happened while it served a link, and nothing older.
    virtual unsigned lostSessions() = 0;

protected:
    ~WirelessHost() {}
};

// Runs until a stop is requested or Bluetooth stops. Each link that comes up gets aa::WirelessSetup with the access
// point and TCP port the phone is told, after a short listen (a MyBMW link speaks first: nothing is written into it);
// once the phone accepts, the link is served until it closes, or until a session is lost while nothing else starts:
// the link is then dropped, the phone opens it again and gets a new handshake (a phone that keeps the link never asks
// again on its own). A link that comes up while a phone connection is being prepared, or while the cable has the phone,
// is held without a new WifiStartRequest until that ends. One that comes up while a session runs over the Wi-Fi gets
// the handshake again, with the Wi-Fi the phone is already on.
void runWirelessLoop(AlpsSppLink& bt, const aa::ApInfo& ap, uint16_t port, WirelessHost& host);

}  // namespace evo

#endif
