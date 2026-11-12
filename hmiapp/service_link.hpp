// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The app's side of the service: starting the supervisor /opt/aa/evo-trial from inside HmiMain3D, its control
// messages (a Unix socket with an abstract name, platform/control_address.hpp), the state the service writes, and the
// stop sequence.
//
// The supervisor borrows the car's Bluetooth, runs the service aa-entrynav and gives the Bluetooth back when it ends.
// Control messages: ping, show, hide, quit, gear <n>, call <0|1>, daynight <n>, drivemode <n>, popup <0|1>.
// Single-threaded (the app thread), but for a reaper thread per start that only waits for the supervisor and logs.
#ifndef AA_ENTRYNAV_HMIAPP_SERVICE_LINK_HPP
#define AA_ENTRYNAV_HMIAPP_SERVICE_LINK_HPP

#include "service_state.hpp"

namespace evo {
namespace app {

// Starts the supervisor, unless it is missing, still running, or was started less than 3 s ago.
void launchService();
// Never waits: a command the service cannot take at once is dropped, and logged once until delivery works again.
void sendToService(const char* message);
// The state file when its process is alive; else "connecting" while the supervisor prepares, "running" when the
// control channel answers without a state file, "idle" otherwise.
ServiceState readServiceState();

// Stop: "quit", and STOP for the supervisor; then, one status poll after the other, SIGTERM at 3 s and SIGKILL at
// 15 s for the session process only. The supervisor always finishes its Bluetooth restore by itself.
void beginStop();
void serviceStop();  // once per status poll
bool stopInProgress();

}  // namespace app
}  // namespace evo

#endif
