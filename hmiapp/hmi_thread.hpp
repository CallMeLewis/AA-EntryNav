// Copyright (c) 2026 CallMeLewis
// Work for the instrument cluster's own HMI thread, asked for from any thread: an event of the app's own, routed to
// the cluster's proc unit.
//
// iDrive runs several HMI proc units, each on its own thread. Each has its own service node in a thread-local pointer
// (libhmiservices.so getSvcNode, THmiTLSPtr), so a service proxy such as CPxyAuxiliaryInput::getServiceProxy(0x70) is
// a different object on each thread. The cluster's menus (CHmiInstrumentClusterApp) run on CHmiICProcUnit's thread
// and read that thread's proxies: work for them has to run there. Read in HmiMain3D and libhmicom.so (202306092158,
// 22 November 2026):
//   - CHmiDispatchInfo::registerEventGroupID(const char*) gives an event group by name: a 16-bit hash.
//   - CHmiProcUnit::doRegisterEvent(target, member function, group, id), vtable slot 9, from any thread: it takes the
//     unit's dispatcher lock (+0x104, the one CHmiProcUnit::dispatch holds), adds the listener, and for a new key sends
//     a CHmiRegisterEvent so that the main dispatcher routes that key to the unit. CHmiDispatcher::_delegateEvent calls
//     the member function pointer {fn, adj} as fn(target + adj / 2, event), so a plain function with adj 0 is a
//     listener.
//   - CHmiProcUnit::sendEvent (slot 5, from any thread) hands an event to the main dispatcher (the unit's +0x33c),
//     which queues it for every unit registered for its key (the word at +4, group << 16 | id).
//   - The cluster's unit is found in the main dispatcher's routing map, by its vtable (CHmiICProcUnit).
// The event carried is one of iDrive's own (CHmiModePlayerSelectEvent::createEvent, as bt_audio.cpp sends), with its
// key replaced by the app's: only the app listens for that key. Requests that come while one is pending are one.
#ifndef AA_ENTRYNAV_HMIAPP_HMI_THREAD_HPP
#define AA_ENTRYNAV_HMIAPP_HMI_THREAD_HPP

namespace evo {
namespace app {

using HmiThreadJob = void (*)();

// In createApp, on HmiMain3D's thread: the app's group, and its listener on the cluster's unit (or later, from
// requests, while that unit is not found yet). job runs on the cluster's thread for each request. Logs what it found;
// false when something was not as expected, and requests then do nothing.
bool captureHmiThread(HmiThreadJob job);

// From any thread: job runs on the cluster's thread soon. False when the listener is not in place or iDrive refused.
bool requestHmiThread();

// Whether job is running on the cluster's thread right now (for checks and logs).
bool onHmiThread();

}  // namespace app
}  // namespace evo

#endif
