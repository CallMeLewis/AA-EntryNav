// Copyright (c) 2026 CallMeLewis
// Which of HmiMain3D's threads may look iDrive's objects up. hmiplugin::get<...>() (CPluginInstanceCacheTLS),
// getHmiProcUnit() and a proxy's getServiceProxy() answer from per-thread state: right on HmiMain3D's own thread
// (createApp's) and on each HMI unit's thread as iDrive dispatches to it, wrong on the bmw::Dispatcher threads that run
// the app's posted tasks. In 0.1.9-test.1 and test.2 one such lookup on the status poll's thread stopped that thread
// and iDrive died after it. Every lookup asks hmiThreadAllowed() first; `make check` (tools/verify_hmi_affinity.py)
// refuses those symbols in a file that does not include this header.
#ifndef AA_ENTRYNAV_HMIAPP_HMI_AFFINITY_HPP
#define AA_ENTRYNAV_HMIAPP_HMI_AFFINITY_HPP

namespace evo {
namespace app {

enum HmiThreadRule {
    kHmiMainThread,  // HmiMain3D's own thread only (createApp's)
    kHmiUnitThread   // a thread iDrive dispatches HMI events on (its own or the cluster's), never a posted task's
};

// Observe build: a lookup on the wrong thread is logged and made anyway, to learn which threads iDrive's hooks really
// run on before refusing anything. Set to true once a car drive shows no such line.
const bool kHmiAffinityEnforced = false;

// In createApp: this thread is HmiMain3D's own.
void rememberHmiMainThread();
bool onHmiMainThread();

// Around each of the app's posted tasks (bmw_framework.cpp): this thread is a bmw::Dispatcher thread.
void enterAppTask(bool inside);

// Whether this thread suits rule.
bool hmiThreadRight(HmiThreadRule rule);

// Before a lookup named call (a literal): whether to make it. Logs the first wrong-thread lookup of each call.
bool hmiThreadAllowed(const char* call, HmiThreadRule rule);

}  // namespace app
}  // namespace evo

#endif
