// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's CDS properties the app follows: gear, the two phone lines, day/night, driving mode. Each change goes to
// the hold machine, and all but the driving mode to the service as a control message.
//   driving.gear                     "gear <n>": in reverse the camera shows and Android Auto steps aside
//   communication.callStatus0 / 1    "call 1" / "call 0": the phone keys follow iDrive's call
//   vehicle.dayNightChangeOver       "daynight <n>" (1 day, 2 night), also written to /tmp/aa-entrynav-daynight
//   driving.mode / driving.mode35up  a real change between two valid modes only: the hold machine sends "drivemode"
//                                    if iDrive's driving mode display then covers our state (popup_peek.hpp)
//   driving.speedActual              kept for the Flare Drive page, which hides its QR code while the car moves
//   vehicle.VIN                      its serial number gives the vehicle id written to contract::kCarFile
//                                    (car_identity.hpp); never logged
//   vehicle.type                     logged only, masked: '{"type":255}', then '{"type":36}' on the reference F20
// They also read the car's coding (car_coding.hpp) once, and write what they found to contract::kCarFile.
// Every handler runs on the app thread.
#ifndef AA_ENTRYNAV_HMIAPP_CAR_SIGNALS_HPP
#define AA_ENTRYNAV_HMIAPP_CAR_SIGNALS_HPP

#include "bmw_framework.hpp"
#include "hold_machine.hpp"

namespace evo {
namespace app {

// Registers the handlers; their changes also go to hold.
void subscribeCarSignals(bmw::PluginApp* app, HoldMachine& hold);
bool callActive();  // either phone line is in a call
int lastGear();     // kGearUnknown until the first driving.gear value
// The car moves: driving.speedActual at kMovingKmh or more. False while no speed has come yet.
const int kMovingKmh = 3;
bool carMoving();

}  // namespace app
}  // namespace evo

#endif
