// Copyright (c) 2026 CallMeLewis
// Where the app is in its status poll and its HMI thread job, for the service to log when the app's heartbeat stops
// (platform/app_heartbeat.hpp). One fixed-size line per slot in contract::kAppStepFile, written in place with pwrite:
// no allocation and no lock, so a step that never returns leaves its name behind.
#ifndef AA_ENTRYNAV_HMIAPP_APP_STEP_HPP
#define AA_ENTRYNAV_HMIAPP_APP_STEP_HPP

namespace evo {
namespace app {

enum AppStepSlot {
    kStepStatus = 0,   // the status poll (a bmw::Dispatcher thread)
    kStepHmiMain = 1,  // the hmi_thread job on HmiMain3D's own thread
    kStepHmiOther = 2  // the hmi_thread job on the cluster's thread
};

// Records that the slot's thread now runs step (a literal, cut to fit the line). The file opens on first use.
void appStep(AppStepSlot slot, const char* step);

// In destroyApp: closes the file.
void closeAppStep();

}  // namespace app
}  // namespace evo

#endif
